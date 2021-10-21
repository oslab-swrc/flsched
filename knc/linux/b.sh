#ctags -R --exclude=drivers --exclude=crypto --exclude=sound --exclude=net --exclude=fs --exclude=virt --exclude=security --exclude=tools 
#make -j128; make -j128 modules_install; make -j128 install
make -j256 bzImage; cp arch/x86/boot/bzImage /boot/vmlinuz-3.10.0.knldefault.flsched; date; ls -aFl /boot/vmlinuz-3.10.*; cp System.map /boot/System.map-3.10.0.knldefault.flsched


