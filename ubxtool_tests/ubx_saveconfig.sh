echo Start UBX save config script

#set up verbosity level & protocol
export UBXOPTS="-v 2 -P 23.01"

#save current NAV configuration (last byte selects for memory to load  from/to)
ubxtool -c 0x06,0x09,0x00,0x00,0x00,0x00,0x0C,0x08,0,0,0x00,0x00,0x00,0x00,0x04

echo UBX save config complete
