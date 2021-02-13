#ifndef _LOG_H
#define _LOG_H
#include <stdio.h>

//TODO add log dumping into file
#define LOG_FILE "/tmp/vd_player.log"
#define LOG_TAG "vd_player"

#define LOGD(fmt, args...) printf("%s::%s:%d " fmt "\n",LOG_TAG,__func__,__LINE__, ##args)

#endif //_LOG_H
