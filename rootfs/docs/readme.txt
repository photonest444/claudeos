ClaudeOS quick reference
========================

This file lives on the FAT file system. The root file system is the
RAM disk loaded by GRUB (changes are lost on reboot), or the hard disk
image (disk.img) if the VM has one attached (changes are kept).

Try these:
  ls /bin              programs you can run
  hello a b c          run a program with arguments
  primes 1000          sieve of Eratosthenes
  guess                number guessing game
  counter 20 &         run in the background, then: ps
  spin                 endless loop; Ctrl+C stops it
  crash                memory protection demo
  wc /docs/readme.txt  read a file through system calls
  echo hi > /home/a.txt ; cat /home/a.txt
  mkdir /home/test ; ls /home ; rm /home/test
  gui                  start the desktop (or press F12)
