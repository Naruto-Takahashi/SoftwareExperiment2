#ifndef MTK_C_H
#define MTK_C_H

/* ======================================
 * 定数定義
 * ====================================== */
#define NULLTASKID     0       /* キューの終端 */
#define NUMTASK        5       /* 最大タスク数 */
#define NUMSEMAPHORE   3       /* セマフォの数*/
#define STKSIZE        4096    /* スタックサイズ (5KB) */

/* タスクの状態 (status) 用の定数例 */
#define UNDEFINED      0       /* 未定義 */
#define READY          1       /* 実行待ち */
#define RUNNING        2       /* 実行中 */
#define WAITING        3       /* 待ち状態 */
#define EXIT           4       /* 終了 */


/* ======================================
 * 型定義
 * ====================================== */

/* タスクIDの型 (実体はint) */
typedef int TASK_ID_TYPE;

/* セマフォ構造体 */
typedef struct {
    int count;
    int nst;                /* reserved */
    TASK_ID_TYPE task_list; /* 待ち行列の先頭タスクID */
} SEMAPHORE_TYPE;

/* TCB (Task Control Block) 構造体 */
typedef struct {
    void (*task_addr)();    /* タスクの開始アドレス (関数ポインタ) */
    void *stack_ptr;        /* スタックポインタ */
    int priority;           /* 優先度 */
    int status;             /* タスクの状態 */
    TASK_ID_TYPE next;      /* キューの次の要素 */
} TCB_TYPE;

/* スタック構造体 */
typedef struct {
    char ustack[STKSIZE];   /* ユーザスタック */
    char sstack[STKSIZE];   /* システムスタック */
} STACK_TYPE;


/* ======================================
 * 大域変数 (extern宣言)
 * ※ 実体は .c ファイルで定義すること
 * ====================================== */

/* セマフォ配列 */
extern SEMAPHORE_TYPE semaphore[NUMSEMAPHORE];

/* TCB配列 (ID=1から使うため +1 する) */
extern TCB_TYPE task_tab[NUMTASK + 1];

/* スタック配列 (ID=1のタスクが stacks[0] を使う) */
extern STACK_TYPE stacks[NUMTASK];

/* システム制御用変数 */
extern TASK_ID_TYPE curr_task;
extern TASK_ID_TYPE new_task;
extern TASK_ID_TYPE next_task;
extern TASK_ID_TYPE ready;

extern volatile unsigned long tick;

#endif /* MTK_C_H */
