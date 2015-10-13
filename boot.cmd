setenv serverip 192.168.0.105
setenv autoload no
dhcp
tftp 0x44000000 ct/boot.scr
source 0x44000000
