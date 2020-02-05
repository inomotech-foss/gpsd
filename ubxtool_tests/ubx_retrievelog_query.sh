#!/bin/bash

echo Start UBX retrievelog_query script

ID=$1

if [ $# -eq 0 ]
  then
    echo "entryID (unit32_t) required as parameter"
    exit 1
fi


if ! [[ "$ID" =~ ^[0-9]+$ ]]
    then
        echo "Sorry entryID must be an integer"
	exit 1
fi

ID1=$((ID & 0x000000FF))
ID1=`printf '%x\n' $ID1`
echo ID1:$ID1
ID2=$((ID & 0x0000FF00))
ID2=$((ID2>>8))
ID2=`printf '%x\n' $ID2`
echo ID2:$ID2
ID3=$((ID & 0x00FF0000))
ID3=$((ID3>>16))
ID3=`printf '%x\n' $ID3`
echo ID3:$ID3
ID4=$((ID & 0xFF000000))
ID4=$((ID4>>24))
ID4=`printf '%x\n' $ID4`
echo ID4:$ID4

#set up verbosity level & protocol
export UBXOPTS="-v 2 -P 23.01"

##disable LOGFILTER
#flags >> recorEnabled, applyAllFilters
#minInterval >> 118s=0x76
#timeThd >> 120s=0x0078
#speedThd >> None=0
#distanceThd >> 500m=0x1F4
#ubxtool -c 0x06,0x47,0x01,0x05,0x76,0x00,0x78,0x00,0,0,0xF4,0x01,0,0

#config w/ 10secs between logs (9sec min time interval, 10sec Thd)
ubxtool -c 0x06,0x47,0x01,0x06,0x09,0x00,0x0A,0x00,0,0,0xF4,0x01,0,0

#retrieve logged data w/ LOG-RETRIEVE (255 entries retrieved from indicated ID)
echo ubxtool -c 0x21,0x09,0x$ID1,0x$ID2,0x$ID3,0x$ID4,0xFF,0,0,0,0,0,0,0
ubxtool -c 0x21,0x09,0x$ID1,0x$ID2,0x$ID3,0x$ID4,0xFF,0,0,0,0,0,0,0

#re-enable LOGFILTER (start logging again)
ubxtool -c 0x06,0x47,0x01,0x07,0x09,0x00,0x0A,0x00,0,0,0xF4,0x01,0,0

echo UBX retrievelog_query complete
exit 0
