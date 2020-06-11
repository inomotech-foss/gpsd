echo Start UBX config script

#set up verbosity level & protocol
export UBXOPTS="-v 2 -P 23.01"

##disable LOGFILTER
#flags >> recorEnabled, applyAllFilters
#minInterval >> 9s=0x09
#timeThd >> 10s=0x0A
#speedThd >> None=0
#distanceThd >> 500m=0x1F4
ubxtool -c 0x06,0x47,0x01,0x06,0x09,0x00,0x0A,0x00,0,0,0xF4,0x01,0,0

#retrieve logged data w/ LOG-RETRIEVE
ubxtool -c 0x21,0x09,0xB0,0,0,0,0xFF,0,0,0,0,0,0,0
#ubxtool -p LOG-RETRIEVE #seem not to operate as expected as of 02.2020 

#re-enable LOGFILTER (start logging again)
ubxtool -c 0x06,0x47,0x01,0x07,0x09,0x00,0x0A,0x00,0,0,0xF4,0x01,0,0

echo UBX config complete
exit 0
