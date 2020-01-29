echo Start UBX save config script

# export UBXOPTS="::/dev/ttyAMA0"
#set up verbosity level
export UBXOPTS="-v 2"

#save current NAV configuration into QSPI (last byte selects for memory to load  from/to)
ubxtool -c 0x06,0x09,0x00,0x00,0x00,0x00,0x0C,0x08,0,0,0x00,0x00,0x00,0x00,0x10

echo UBX save config complete
