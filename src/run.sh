#!/bin/bash
killall ipush;
rm -rf /data/ipush;
#gcc -o wp worker.c wtable.c table.c utils/*.c -I utils -lpthread -levbase -DUSE_PTHREAD && nohup ./wp 2188 /data/wtab & 
gcc -o ipush worker.c wtable.c utils/*.c -I utils -lpthread -levbase -DHAVE_SSL && ./ipush --port=8253  --whitelist="127.0.0.1,192.168.0.1" --workdir=/data/ipush --cert=/data/xhttpd/yunall/push.yunall.co.crt --privkey=/data/xhttpd/yunall/push.yunall.co.key --ssl --daemon 
##sleep 4
##curl -s 'http://127.0.0.1:2188/q?op=reload&dir=/data/table&dict=/data/.devel/qss' ;

