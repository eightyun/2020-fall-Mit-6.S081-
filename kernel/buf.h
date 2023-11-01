struct buf {
  int valid;   // has data been read from disk?   表示缓冲区是否包含块的副本
  int disk;    // does disk "own" buf?  表示缓冲区内容是否已交给磁盘
  uint dev;
  uint blockno;
  struct sleeplock lock;
  uint refcnt;
  struct buf *prev; // LRU cache list
  struct buf *next;
  uchar data[BSIZE];
  uint timestamp;  // 时间戳
};

