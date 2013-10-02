#!/bin/bash
killall -9 w;
killall -9 wp;
rm -rf /data/wtab /data/table;
#gcc -o wp worker.c wtable.c table.c utils/*.c -I utils -lpthread -levbase -DUSE_PTHREAD && nohup ./wp 2188 /data/wtab & 
gcc -o wp worker.c wtable.c table.c utils/*.c -I utils -lpthread -levbase && nohup ./wp 2188 /data/wtab &
sleep 4
curl -s 'http://127.0.0.1:2188/q?op=reload&dir=/data/table&dict=/data/.devel/qss' ;

