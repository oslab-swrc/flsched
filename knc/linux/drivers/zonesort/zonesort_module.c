/*
 * **
 * ** Copyright 2017 by Intel Corporation.
 * **
 * ** This program is free software; you can redistribute it and/or
 * ** modify it under the terms of the GNU General Public License
 * ** as published by the Free Software Foundation; either version 2
 * ** of the License, or (at your option) any later version.
 * **
 * ** This program is distributed in the hope that it will be useful,
 * ** but WITHOUT ANY WARRANTY; without even the implied warranty of
 * ** MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * ** GNU General Public License for more details.
 * **
 * */

/* *
 * The module provides mechanism to change order in a list of
 * free pages kept in Linux kernel buddy allocator.
 * The order is changed so to minimize possible page collisions
 * later when those pages are served to the application.
 * The algorithm assumes that the system is supported by 16 GB of
 * direct mapped cache.
 * */

#include <linux/init.h>
#include <linux/numa.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/mm.h>
#include <linux/list_sort.h>
#include <linux/debugfs.h>
#include <linux/seq_file.h>
#include <linux/vmalloc.h>
#include <linux/mutex.h>
#include <linux/workqueue.h>

#define MODNAME "zonesort"

#define err(format, arg...) pr_err(MODNAME ": " format, ## arg)
#define debug(format, arg...) pr_debug(MODNAME ": " format, ## arg)
#define info(format, arg...) pr_info(MODNAME ": " format, ## arg)
#define warn(format, arg...) pr_warn(MODNAME ": " format, ## arg)

#ifndef CONFIG_SYSFS
#error "This module requires CONFIG_SYSFS"
#endif

#define KB(x)   ((x) * 1024ull)
#define MB(x)   (KB (KB (x)))
#define GB(x)   (MB (KB (x)))

#define COLLISION_GRANULARITY_ORDER (MAX_ORDER - 2)
// expressed as multiply of 4KB page size
#define COLLISION_GRANULARITY (1l*(1<<COLLISION_GRANULARITY_ORDER)*KB(4))
// defines number of buckets for pages
#define MAX_BAD_IDX 4

/*
 * GLOBALS
 */

static struct workqueue_struct *sort_queue;

static void _sort_work_fn(struct work_struct *work);
static DECLARE_DELAYED_WORK(sort_work, _sort_work_fn);
static DEFINE_MUTEX(sort_lock);
static unsigned long sort_interval_msecs;

static u64 list_to_phys(struct list_head* l)
{
	struct page* p = list_entry(l, struct page, lru);
	return page_to_phys(p);
}

static int cmp_pages(void *priv, struct list_head *a, struct list_head *b)
{
	/*
	 * We just need to compare the pointers.  The 'struct
	 * page' with vmemmap are ordered in the virtual address
	 * space by physical address.  The list_head is embedded
	 * in the 'struct page'.  So we don't even have to get
	 * back to the 'struct page' here.
	 */
	if (a < b)
		return -1;
	if (a == b)
		return 0;
	/* a > b */
	return 1;
}

/*
 * SORTING ALGORITHM
 */

// each list represents bucket for pages
// index of the list defines how 'bad' the pages are:
// [no collision, 1 collision, 2 collistion, more]
static struct list_head bad_list[MAX_BAD_IDX];
static int bad_list_size[MAX_BAD_IDX];
// array of pages of maximal size fitting in cache
static u32 collisions[GB(16) / COLLISION_GRANULARITY];

static void sort_init(void)
{
	int i;

	memset(bad_list_size, 0, sizeof(bad_list_size));
	for (i=0; i<MAX_BAD_IDX; ++i)
		INIT_LIST_HEAD(&bad_list[i]);
}

static int compute_bad_idx(int idx)
{
	// empirically determined threshold values
	u32 v = collisions[idx];
	if (v == 0)						return 0;
	if (v < (1 << COLLISION_GRANULARITY_ORDER))		return 1;
	if (v < (1 << COLLISION_GRANULARITY_ORDER) * 2)		return 2;
	return 3;
}

static void sort_movable_order(struct zone *zone, int order, struct list_head *list)
{
	int i, idx, bad_idx;
	unsigned long flags, phys;
	struct list_head *pos, *tmp;

	struct list_head *free_l = &zone->free_area[order].free_list[MIGRATE_MOVABLE];
	sort_init();

	spin_lock_irqsave(&zone->lock, flags);
	for (i=0; i < order; ++i) {
		struct list_head *li = &zone->free_area[i].free_list[MIGRATE_MOVABLE];

		list_for_each (pos, li) {
			phys = list_to_phys(pos);
			idx = (phys % GB(16)) / COLLISION_GRANULARITY;

			collisions[idx] += (1 << i);
		}
	}

	// walk choosen order, update collisions and distribute to less & more
	// heaviliy occupied bad lists
	list_for_each_safe (pos, tmp, free_l) {
		phys = list_to_phys(pos);
		idx = (phys % GB(16)) / COLLISION_GRANULARITY;

		bad_idx = compute_bad_idx(idx);

		// occupy two pages
		collisions[idx + 0] += (1 << COLLISION_GRANULARITY_ORDER);
		collisions[idx + 1] += (1 << COLLISION_GRANULARITY_ORDER);

		list_del_init(pos);
		list_add(pos, &bad_list[bad_idx]);
		bad_list_size[bad_idx]++;
	}

	// at this point, free list should be empty
	debug("list_empty(free_l) == %d\n", (int)list_empty(free_l));

	// re-create free list of chosen order
	for (i=0; i< MAX_BAD_IDX; ++i) {
		list_splice_tail(&bad_list[i], free_l);
		debug("list %d size = %d\n", i, bad_list_size[i]);
	}

	spin_unlock_irqrestore(&zone->lock, flags);
}


static void sort_pagelists(struct zone *zone)
{
	unsigned int order;
	unsigned int type;
	unsigned long flags;

	for_each_migratetype_order(order, type) {
		// operations on internal kernel lists are protected by spinlock
		struct list_head *l = &zone->free_area[order].free_list[type];
		if (type == MIGRATE_MOVABLE && order == MAX_ORDER - 1) {
			sort_movable_order(zone, order, l);
		}
		else {
			spin_lock_irqsave(&zone->lock, flags);
			list_sort(NULL, l, &cmp_pages);
			spin_unlock_irqrestore(&zone->lock, flags);
		}
	}

}

/*
 * BUDDY LOG DEBUG IFACE
 */

static int buddy_list_seq_show(struct seq_file* file, void* data)
{
        unsigned int order;
        unsigned int type;
        unsigned long flags;

	int max_size = 1024*1024;
	u64* buffer = vmalloc(max_size * sizeof(u64));
	struct list_head *iter;
	int i, j, entries;
	struct zone *zone = NULL;
	unsigned int node_id = 0;

	for (node_id = 0; node_id < MAX_NUMNODES; node_id++) {
		if (!node_online(node_id))
			continue;

		memset(buffer, '\3', max_size * sizeof(u64));

		for (i = 0; i < MAX_NR_ZONES; i++) {
			zone = &NODE_DATA(node_id)->node_zones[i];

			if (!zone_is_initialized(zone) || !populated_zone(zone))
				continue;

			for_each_migratetype_order(order, type) {
				struct list_head *l = &zone->free_area[order].free_list[type];

				entries = 0;

				spin_lock_irqsave(&zone->lock, flags);
				list_for_each(iter, l) {
					buffer[entries++] = list_to_phys(iter);
					if (entries == max_size)
						break;
				}
				spin_unlock_irqrestore(&zone->lock, flags);

				seq_printf(file, "Node id: %d, zone: %d, order: %d, type %d, total pages: %d\n",
						(int)node_id, (int)i, (int)order, (int)type, (int)entries);
				for (j=0; j<entries; j++)
					seq_printf(file, "%p\n", (void *)buffer[j]);
			}
		}
	}

	vfree(buffer);
	return 0;
}

struct dentry* buddy_lists;

static int buddy_list_open(struct inode* inode, struct file* file)
{
	return single_open(file, buddy_list_seq_show, NULL);
}

struct file_operations buddy_lists_fops = {
        .owner   = THIS_MODULE,
        .open    = buddy_list_open,
        .read    = seq_read,
        .llseek  = seq_lseek,
        .release = single_release
};

/*
 * DIRECT MAPPED CACHE DEBUG IFACE
 */

static char * const dmc_migratetype_names[MIGRATE_TYPES] = {
	"Unmovable",
	"Reclaimable",
	"Movable",
	"Reserve",
#ifdef CONFIG_CMA
	"CMA",
#endif
#ifdef CONFIG_MEMORY_ISOLATION
	"Isolate",
#endif
};

/* KNL: 16GB of memory size cache 16GB */
#define DIRECTMAPPEDCACHE_SHIFT (34)
#define DIRECTMAPPEDCACHE_SIZE (1L << DIRECTMAPPEDCACHE_SHIFT)
#define DIRECTMAPPEDCACHE_2MB_PAGE_SHIFT (21)
#define DIRECTMAPPEDCACHE_2MB_TRACER_TAB_SHIFT (DIRECTMAPPEDCACHE_SHIFT - DIRECTMAPPEDCACHE_2MB_PAGE_SHIFT)
#define DIRECTMAPPEDCACHE_2MB_TRACER_TAB_SIZE (1L << DIRECTMAPPEDCACHE_2MB_TRACER_TAB_SHIFT)
#define DIRECTMAPPEDCACHE_MAX_LEVEL (8)

static unsigned int directmappedcache_tracker_tab[DIRECTMAPPEDCACHE_2MB_TRACER_TAB_SIZE] = { 0 };

static void directmappedcache_show_tracker_status(struct seq_file* file, unsigned long entries, unsigned int order, unsigned int type)
{
	unsigned long over_max = 0;
	char line[300];
	unsigned long i;
	int written = 0;

	if (order >= (DIRECTMAPPEDCACHE_2MB_PAGE_SHIFT - PAGE_SHIFT)) {
		unsigned long level[DIRECTMAPPEDCACHE_MAX_LEVEL];
		memset(level, 0, sizeof(level));
		for (i = 0; i < DIRECTMAPPEDCACHE_2MB_TRACER_TAB_SIZE; i++) {
			if (directmappedcache_tracker_tab[i] >= DIRECTMAPPEDCACHE_MAX_LEVEL) {
				over_max ++;
				seq_printf(file, "      over_max idx[%4lu] = %u\n", i, directmappedcache_tracker_tab[i]);
			} else {
				level[directmappedcache_tracker_tab[i]]++;
			}
		}
		written += scnprintf(line + written, sizeof(line) - written,
				"      [%3u ][%-12s] %7lu:  ",
				order, dmc_migratetype_names[type], entries);
		for (i = 0; i < DIRECTMAPPEDCACHE_MAX_LEVEL; i++) {
			written += scnprintf(line + written, sizeof(line) - written,  "%6lu ", level[i]);
		}
		if ( over_max > 0) {
			written += scnprintf(line + written, sizeof(line) - written, "over_max %6lu", over_max);
		}
		seq_printf(file, "%s\n",line);
	} else {
		seq_printf(file, "Not implemented yet for: %s\n",line);
	}
}


static void directmappedcache_pagelists_show(struct seq_file* file, struct zone *zone)
{
	unsigned int order;
	unsigned int type;
	unsigned long free_count_table[MAX_ORDER][MIGRATE_TYPES] = {{0}};
	char line[120];
	unsigned int show_tracker_title = 0;
	int written = 0;

	for_each_migratetype_order(order, type) {
		struct list_head *l = &zone->free_area[order].free_list[type];
		unsigned long free_count = 0;
		struct list_head *curr;

		/* for order equal or greater than 9 (2MB) recalculate colission */
		if (order >= (DIRECTMAPPEDCACHE_2MB_PAGE_SHIFT - PAGE_SHIFT))
		{

			unsigned long pos_per_order;
			unsigned long analyzed_entries;
			int my_error_log = 0;

			pos_per_order = 1 << (order - (DIRECTMAPPEDCACHE_2MB_PAGE_SHIFT - PAGE_SHIFT));
			analyzed_entries = 0;

			memset(directmappedcache_tracker_tab, 0, sizeof(directmappedcache_tracker_tab));

			list_for_each(curr, l) {
				unsigned long index;
				unsigned int detect_used = 0;
				unsigned long i;
				unsigned long pfn;

				if (show_tracker_title == 0) {
					seq_printf(file, "  DIRECT MAPPED CACHE STATE:\n");
					seq_printf(file, "      order [type        ] entries:   empty      1      2  \n");
					show_tracker_title = 1;
				}

				pfn = page_to_pfn(list_entry(curr, struct page, lru));
				index = (pfn >> (DIRECTMAPPEDCACHE_2MB_PAGE_SHIFT - PAGE_SHIFT))
					& ((1lu << DIRECTMAPPEDCACHE_2MB_TRACER_TAB_SHIFT)-1);

				/* find max used entry */
				for (i=0; i < pos_per_order; i++) {
					if (directmappedcache_tracker_tab[i+index] > detect_used) {
						detect_used = directmappedcache_tracker_tab[i+index];
					}
				}
				/* use +1 regarding to already used level */
				detect_used++;
				if ((detect_used >= DIRECTMAPPEDCACHE_MAX_LEVEL) && (my_error_log<10)) {
					/* something wrong with the page */
					seq_printf(file, "   --> ERR: pfn %lx idx %lu detect_used %u \n", pfn, index, detect_used);
					my_error_log ++;
				}
				for (i=0; i < pos_per_order; i++) {
					directmappedcache_tracker_tab[i+index] = detect_used;
				}
				analyzed_entries += pos_per_order;
				if ((analyzed_entries & (DIRECTMAPPEDCACHE_2MB_TRACER_TAB_SIZE-1)) == 0) {
					directmappedcache_show_tracker_status(file, analyzed_entries, order, type);
				}
				free_count++;
			}
			if (analyzed_entries) {
				directmappedcache_show_tracker_status(file, analyzed_entries, order, type );
			}

		} else {
			list_for_each(curr, l) {
				free_count++;
			}
		}

		free_count_table[order][type] = free_count;
	}
	seq_printf(file, "  SUMMARY:\n");
	for (type = 0; type < MIGRATE_TYPES; type++) {
		written = 0;
		written += scnprintf(line + written, sizeof(line) - written,
				"  %-12s:",	dmc_migratetype_names[type]);
		for (order = 0; order < MAX_ORDER; ++order) {
			written += scnprintf(line + written, sizeof(line) - written , "%6lu ", free_count_table[order][type]);
		}
		seq_printf(file, "    %s\n", line);
	}
}

static int directmappedcache_state_seq_show(struct seq_file* file, void* data)
{
	struct zone *zone = NULL;
	int i = 0;
	unsigned int node_id = 0;

	for (node_id = 0; node_id < MAX_NUMNODES; node_id++) {
		if (!node_online(node_id))
			continue;

		seq_printf(file, "DIRECTMAPPEDCACHE Show node %u, max zones %u\n", node_id, MAX_NR_ZONES);
		for (i = 0; i < MAX_NR_ZONES; i++) {
			zone = &NODE_DATA(node_id)->node_zones[i];
			if (!zone_is_initialized(zone)) {
				seq_printf(file, "Zone %d is not initialized\n", i);
				continue;
			}
			if (!populated_zone(zone)) {
				seq_printf(file, "Zone %d is not populated\n", i);
				continue;
			}
			seq_printf(file, "Zone %d (%s) to analyze\n", i, zone->name);
			directmappedcache_pagelists_show(file, zone);
		}
	}
	return 0;
}

struct dentry* directmappedcache_state_debugfs;

static int directmappedcache_state_open(struct inode* inode, struct file* file)
{
	return single_open(file, directmappedcache_state_seq_show, NULL);
}

struct file_operations directmappedcache_state_fops = {
	.owner   = THIS_MODULE,
	.open    = directmappedcache_state_open,
	.read    = seq_read,
	.llseek  = seq_lseek,
	.release = single_release
};

/*
 * MODULE COMMON
 */

static void sort_node(unsigned int nodeid)
{
	int i;
	struct zone *zone = NULL;

	memset(collisions, 0, sizeof(collisions));
	for (i = 0; i < MAX_NR_ZONES; i++) {
		zone = &NODE_DATA(nodeid)->node_zones[i];
		if (!zone_is_initialized(zone)) {
			info("Zone %d is not initialized\n", i);
			continue;
		}
		if (!populated_zone(zone)) {
			info("Zone %d is not populated\n", i);
			continue;
		}
		sort_pagelists(zone);
	}
}

void _sort_work_fn(struct work_struct *work)
{
	unsigned int nid;
	int ret;

	// skip if previous sorting did not finish
	if(mutex_trylock(&sort_lock)) {
		for_each_online_node(nid) {
			info("Sorting node %u\n", nid);
			sort_node(nid);
		}
		mutex_unlock(&sort_lock);
	} else {
		warn("periodic sorting skipped, consider increasing sorting interval.\n");
	}
	// reschedule work
	if (sort_interval_msecs != 0) {
		ret = queue_delayed_work(sort_queue, &sort_work, msecs_to_jiffies(sort_interval_msecs));
		// should never happen, we have only one work
		if (!ret)
			warn("internal error\n");
	}
}

static ssize_t sort_interval_show(struct kobject *kobj, struct kobj_attribute *attr, char *buf)
{
	return sprintf(buf, "%lu sec\n", sort_interval_msecs/1000);
}

static ssize_t sort_interval_store(struct kobject *kobj, struct kobj_attribute *attr, const char *buf, size_t count)
{
	unsigned int sort_interval_sec;

	if (sscanf(buf, "%u", &sort_interval_sec) != 1)
		return -EINVAL;

	if (sort_interval_sec == 0 && sort_interval_msecs != 0)
		info("periodic sort delay turned off\n");

	sort_interval_msecs = sort_interval_sec * 1000;

	if (sort_interval_sec == 0) {
		// return value ignored on purpose
		cancel_delayed_work_sync(&sort_work);
	} else {
		info("periodic sorting interval: %d sec\n", sort_interval_sec);
		// also queues work if it was idle
		// return value ignored on purpose
		mod_delayed_work(sort_queue, &sort_work, msecs_to_jiffies(sort_interval_msecs));
	}

	return count;
}

static struct kobj_attribute sort_interval_attribute =
	__ATTR(sort_interval, 0664, sort_interval_show, sort_interval_store);

static ssize_t nodeid_show(struct kobject *kobj, struct kobj_attribute *attr, char *buf)
{
	return -EPERM;
}

static ssize_t nodeid_store(struct kobject *kobj, struct kobj_attribute *attr, const char *buf, size_t count)
{
	unsigned int nodeid;

	if (sscanf(buf, "%u", &nodeid) != 1)
		return -EINVAL;

	if (nodeid >= MAX_NUMNODES || !node_online(nodeid)) {
		info("Node %u is not online\n", nodeid);
		return -EINVAL;
	}

	if (sort_interval_msecs != 0)
		return -EBUSY;

	info("Sorting node %u\n", nodeid);
	if (mutex_trylock(&sort_lock)) {
		sort_node(nodeid);
		mutex_unlock(&sort_lock);
	} else {
		return -EBUSY;
	}

	return count;
}

static struct kobj_attribute nodeid_attribute =
	__ATTR(nodeid, 0664, nodeid_show, nodeid_store);

static struct attribute *attrs[] = {
	&nodeid_attribute.attr,
	&sort_interval_attribute.attr,
	NULL,
};

static struct attribute_group attr_group = {
	.attrs = attrs,
};

static struct kobject *zonesort_kobject;

static int __init m_init(void)
{
	int ret;

	info("init\n");

	sort_queue = create_singlethread_workqueue(MODNAME " queue");
	if (!sort_queue)
		goto fatal;

	zonesort_kobject = kobject_create_and_add("zone_sort_free_pages", kernel_kobj);
	if (!zonesort_kobject)
		goto fatal;

	ret = sysfs_create_group(zonesort_kobject, &attr_group);

	buddy_lists = debugfs_create_file("buddy_lists", 0444, NULL, NULL, &buddy_lists_fops);
	if (ret || !buddy_lists)
		goto clean_zonesort_free_iface;

	directmappedcache_state_debugfs = debugfs_create_file("directmappedcache_state", 0444, NULL, NULL, &directmappedcache_state_fops);
	if (!directmappedcache_state_debugfs)
		goto clean_buddy_iface;

	return 0;

	clean_buddy_iface:
		debugfs_remove(buddy_lists);
	clean_zonesort_free_iface:
		kobject_put(zonesort_kobject);
	fatal:
		warn("Failed to create sysfs or debugfs file\n");
		return -ENOMEM;
}
static void __exit m_exit(void)
{
	sort_interval_msecs = 0;
	// cancel if pending
	cancel_delayed_work_sync(&sort_work);
	// wait to finish
	flush_workqueue(sort_queue);
	destroy_workqueue(sort_queue);

	kobject_put(zonesort_kobject);
	debugfs_remove(buddy_lists);
	debugfs_remove(directmappedcache_state_debugfs);

	info("exit\n");
}

module_init(m_init);
module_exit(m_exit);
MODULE_LICENSE("GPL v2");
MODULE_AUTHOR("Intel");
MODULE_DESCRIPTION("Zone's free list sorter\n");

