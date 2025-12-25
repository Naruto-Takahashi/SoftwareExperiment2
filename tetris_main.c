/* ===========================================================================
 * ファイル名: tetris_main.c
 * 作成者: 高橋成翔
 * 最終更新日: 2025/12/25/11:08
 *
 * [概要]
 * MC68VZ328用マルチタスクカーネル上で動作する2人対戦テトリス．
 *
 * [実装されたマルチタスク構造: 生産者・消費者モデル]
 * 1. 生産者 (Game Task 1 & 2):
 * - ゲームのロジック計算を行う．
 * - 画面更新が必要な場合，直接描画せず ｢描画コマンド｣ をキューに積む．
 * 2. 消費者 (Render Task):
 * - 描画コマンドキューを監視し，データがあれば取り出してUARTに出力する．
 * - I/O待ちの間，計算タスクにCPU時間を譲ることができる．
 *
 * [使用するセマフォと資源]
 * - SEM_GARBAGE (ID 0): お邪魔ライン変数の排他制御 (Mutex)
 * - SEM_RENDER_MUTEX (ID 1): 描画キュー操作の排他制御 (Mutex)
 * - SEM_RENDER_COUNT (ID 2): 描画キュー内のデータ数管理 (Counting)
 * =========================================================================== */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h> /* vsprintf用 */
#include "mtk_c.h"

/* ===========================================================================
 * 外部関数・変数の宣言
 * =========================================================================== */
extern FILE *com0in, *com0out;
extern FILE *com1in, *com1out;

extern void init_kernel(void);
extern void set_task(void (*func)());
extern void begin_sch(void);
extern int inbyte(int ch);
extern void skipmt(void);
extern void P(int sem_id);
extern void V(int sem_id);

extern volatile unsigned long tick;
extern SEMAPHORE_TYPE semaphore[NUMSEMAPHORE];

/* ===========================================================================
 * 定数・マクロ定義
 * =========================================================================== */

/* --- セマフォID割り当て --- */
#define SEM_GARBAGE      0  /* お邪魔ライン共有変数の保護 (Mutex) */
#define SEM_RENDER_MUTEX 1  /* 描画リングバッファの排他制御 (Mutex) */
#define SEM_RENDER_COUNT 2  /* 描画データの残数カウント (Counting) */

/* --- 描画キュー設定 --- */
#define RENDER_QUEUE_SIZE 64  /* リングバッファのサイズ */
#define CMD_STR_LEN       32  /* 1コマンドあたりの最大文字列長 */

/* --- ゲーム設定 --- */
#define FIELD_WIDTH  12       /* フィールド幅 (壁含む) */
#define FIELD_HEIGHT 22       /* フィールド高さ (底含む) */
#define MINO_WIDTH   4        /* ミノデータの幅 */
#define MINO_HEIGHT  4        /* ミノデータの高さ */
#define OPPONENT_OFFSET_X 40  /* 相手画面の描画Xオフセット */

/* --- タイミング設定 (単位: tick = 10ms想定) --- */
#define DROP_INTERVAL 500         /* 自動落下間隔 */
#define ANIMATION_DURATION 3      /* ライン消去アニメーション時間 */
#define COUNTDOWN_DELAY 1000      /* 開始前カウントダウン待機 */
#define DISPLAY_POLL_INTERVAL 50  /* 入力待ち時の画面更新ポーリング間隔 */

/* --- バッファ値定義 --- */
#define CELL_EMPTY  0
#define CELL_WALL   1
#define CELL_GHOST  10

/* --- エスケープシーケンス (VT100互換) --- */
#define ESC_CLS        "\x1b[2J"   /* 画面消去 */
#define ESC_HOME       "\x1b[H"    /* カーソルホーム */
#define ESC_RESET      "\x1b[0m"   /* 属性リセット */
#define ESC_HIDE_CUR   "\x1b[?25l" /* カーソル非表示 */
#define ESC_SHOW_CUR   "\x1b[?25h" /* カーソル表示 */
#define ESC_CLR_LINE   "\x1b[K"    /* 行消去 */
#define ESC_INVERT_ON  "\x1b[?5h"  /* 画面反転 (フラッシュ用) */
#define ESC_INVERT_OFF "\x1b[?5l"  /* 画面反転解除 */

/* --- 色定義 (TrueColor/ANSI) --- */
#define COL_CYAN     "\x1b[38;2;0;255;255m"
#define COL_YELLOW   "\x1b[38;2;255;255;0m"
#define COL_PURPLE   "\x1b[38;2;160;32;240m"
#define COL_BLUE     "\x1b[38;2;0;0;255m"
#define COL_ORANGE   "\x1b[38;2;255;165;0m"
#define COL_GREEN    "\x1b[38;2;0;255;0m"
#define COL_RED      "\x1b[38;2;255;0;0m"
#define COL_WHITE    "\x1b[38;2;255;255;255m"
#define COL_GRAY     "\x1b[38;2;128;128;128m"
#define COL_WALL     COL_WHITE
#define BG_BLACK     "\x1b[40m"

/* ===========================================================================
 * 型定義
 * =========================================================================== */

/* 描画コマンド構造体 (リングバッファの要素) */
typedef struct {
    int port_id;            /* 出力先ポート (0 or 1) */
    char str[CMD_STR_LEN];  /* 出力文字列 (エスケープシーケンス含む) */
} RenderCommand;

/* ゲーム状態列挙 */
typedef enum { 
    GS_PLAYING,   /* プレイ中 */
    GS_ANIMATING, /* ライン消去アニメーション中 */
    GS_GAMEOVER   /* ゲームオーバー */
} GameState;

/* イベント列挙 */
typedef enum { 
    EVT_NONE,      /* イベントなし */
    EVT_KEY_INPUT, /* キー入力あり */
    EVT_TIMER,     /* 落下タイマ満了 */
    EVT_WIN,       /* 勝利 */
    EVT_QUIT       /* 終了 (qキー) */
} EventType;

/* イベント構造体 */
typedef struct {
    EventType type; /* イベントの種類 */
    int param;      /* 付加情報 (キーコード等) */
} Event;

/* ゲームコンテキスト (各タスクの状態保持用) */
typedef struct {
    int port_id;    /* 担当するシリアルポートID */
    
    /* フィールドデータ */
    char field[FIELD_HEIGHT][FIELD_WIDTH];          /* 現在のフィールド */
    char displayBuffer[FIELD_HEIGHT][FIELD_WIDTH];  /* 描画用バッファ (合成後) */
    char prevBuffer[FIELD_HEIGHT][FIELD_WIDTH];     /* 前回描画した内容 (差分更新用) */
    
    /* 相手フィールド用 (差分更新用) */
    char prevOpponentBuffer[FIELD_HEIGHT][FIELD_WIDTH]; 
    int opponent_was_connected; /* 相手接続フラグ */

    /* 進行状態 */
    GameState state;
    unsigned long anim_start_tick;
    int lines_to_clear;

    /* ミノ情報 */
    int minoType;   /* 現在のミノ種類 */
    int minoAngle;  /* 現在の回転角 */
    int minoX, minoY;
    int nextMinoType;
    int prevNextMinoType;
    
    /* 7-Bag ランダム生成用 */
    int bag[7];
    int bag_index;

    /* タイミング・スコア */
    unsigned long next_drop_time;
    int seq_state;  /* エスケープシーケンス解析用状態 */
    int score;
    int lines_cleared;
    
    /* 共有・フラグ系 */
    volatile int pending_garbage; /* 受け取ったお邪魔ライン (要排他制御) */
    volatile int is_gameover;     /* ゲームオーバーフラグ */
    volatile int sync_generation; /* 再戦同期用世代カウンタ */
} TetrisGame;

/* 全ゲームインスタンスへのポインタ (相手参照用) */
TetrisGame *all_games[2] = {NULL, NULL};

/* ===========================================================================
 * グローバル変数 (描画エンジン用)
 * =========================================================================== */

/* 描画コマンドキュー (共有資源) */
RenderCommand render_queue[RENDER_QUEUE_SIZE];
int render_head = 0; /* 書き込み位置 (生産者が操作) */
int render_tail = 0; /* 読み出し位置 (消費者が操作) */

/* ===========================================================================
 * 定数データ (ミノ定義)
 * =========================================================================== */
enum { MINO_I, MINO_O, MINO_S, MINO_Z, MINO_J, MINO_L, MINO_T, MINO_GARBAGE, MINO_MAX };
enum { ANG_0, ANG_90, ANG_180, ANG_270, ANG_MAX };

const char* minoColors[MINO_MAX] = { 
    COL_CYAN, COL_YELLOW, COL_GREEN, COL_RED, COL_BLUE, COL_ORANGE, COL_PURPLE, COL_GRAY 
};

/* ミノ形状定義 [種類][角度][Y][X] */
char minoShapes[MINO_MAX][ANG_MAX][MINO_HEIGHT][MINO_WIDTH] = {
    /* I-Mino */
    { {{0,1,0,0},{0,1,0,0},{0,1,0,0},{0,1,0,0}}, {{0,0,0,0},{0,0,0,0},{1,1,1,1},{0,0,0,0}}, {{0,0,1,0},{0,0,1,0},{0,0,1,0},{0,0,1,0}}, {{0,0,0,0},{1,1,1,1},{0,0,0,0},{0,0,0,0}} },
    /* O-Mino */
    { {{0,0,0,0},{0,1,1,0},{0,1,1,0},{0,0,0,0}}, {{0,0,0,0},{0,1,1,0},{0,1,1,0},{0,0,0,0}}, {{0,0,0,0},{0,1,1,0},{0,1,1,0},{0,0,0,0}}, {{0,0,0,0},{0,1,1,0},{0,1,1,0},{0,0,0,0}} },
    /* S-Mino */
    { {{0,0,0,0},{0,1,1,0},{1,1,0,0},{0,0,0,0}}, {{0,1,0,0},{0,1,1,0},{0,0,1,0},{0,0,0,0}}, {{0,0,0,0},{0,0,1,1},{0,1,1,0},{0,0,0,0}}, {{0,0,0,0},{0,1,0,0},{0,1,1,0},{0,0,1,0}} },
    /* Z-Mino */
    { {{0,0,0,0},{1,1,0,0},{0,1,1,0},{0,0,0,0}}, {{0,0,1,0},{0,1,1,0},{0,1,0,0},{0,0,0,0}}, {{0,0,0,0},{0,1,1,0},{0,0,1,1},{0,0,0,0}}, {{0,0,0,0},{0,0,1,0},{0,1,1,0},{0,1,0,0}} },
    /* J-Mino */
    { {{0,0,1,0},{0,0,1,0},{0,1,1,0},{0,0,0,0}}, {{0,0,0,0},{0,1,0,0},{0,1,1,1},{0,0,0,0}}, {{0,0,0,0},{0,1,1,0},{0,1,0,0},{0,1,0,0}}, {{0,0,0,0},{1,1,1,0},{0,0,1,0},{0,0,0,0}} },
    /* L-Mino */
    { {{0,1,0,0},{0,1,0,0},{0,1,1,0},{0,0,0,0}}, {{0,0,0,0},{0,1,1,1},{0,1,0,0},{0,0,0,0}}, {{0,0,0,0},{0,1,1,0},{0,0,1,0},{0,0,1,0}}, {{0,0,0,0},{0,0,1,0},{1,1,1,0},{0,0,0,0}} },
    /* T-Mino */
    { {{0,0,0,0},{1,1,1,0},{0,1,0,0},{0,0,0,0}}, {{0,0,1,0},{0,1,1,0},{0,0,1,0},{0,0,0,0}}, {{0,0,0,0},{0,0,1,0},{0,1,1,1},{0,0,0,0}}, {{0,0,0,0},{0,1,0,0},{0,1,1,0},{0,1,0,0}} },
    /* Garbage */
    { {{0}},{{0}},{{0}},{{0}} }
};

/* ===========================================================================
 * 描画エンジン (消費者タスク) 関連
 * =========================================================================== */

/* ---------------------------------------------------------------------------
 * 関数名: send_draw_command
 * 概要  : 書式付き文字列を作成し，描画キューに投入する (生産者側処理)．
 * 引数  : port_id - 出力先ポート
 * fmt     - 書式文字列 (printf互換)
 * ...     - 可変引数
 * --------------------------------------------------------------------------- */
void send_draw_command(int port_id, const char *fmt, ...) {
    char buf[CMD_STR_LEN];
    va_list args;
    int next_head;

    /* 文字列フォーマット */
    va_start(args, fmt);
    vsprintf(buf, fmt, args); /* 注意: バッファオーバーフローに注意 */
    va_end(args);

    /* 1. キューに空きがあるか確認 (なければ譲る) */
    while (1) {
        next_head = (render_head + 1) % RENDER_QUEUE_SIZE;
        if (next_head != render_tail) {
            break; /* 空きあり */
        }
        skipmt(); /* バッファフルなら他タスクへ */
    }

    /* 2. クリティカルセクション: キューへの書き込み */
    P(SEM_RENDER_MUTEX);
    
    render_queue[render_head].port_id = port_id;
    strncpy(render_queue[render_head].str, buf, CMD_STR_LEN - 1);
    render_queue[render_head].str[CMD_STR_LEN - 1] = '\0'; // 安全のため
    render_head = next_head;
    
    V(SEM_RENDER_MUTEX);

    /* 3. 消費者へ通知 (データ数+1) */
    V(SEM_RENDER_COUNT);
}

/* ---------------------------------------------------------------------------
 * 関数名: task_render
 * 概要  : 描画専門タスク (消費者)
 * 詳細  : SEM_RENDER_COUNTで待機し，データがあればUARTへ出力する．
 * I/O処理を分離することでゲームロジックの遅延を防ぐ．
 * --------------------------------------------------------------------------- */
void task_render(void) {
    RenderCommand cmd;
    FILE *target_fp;

    while (1) {
        /* 1. データが来るまでスリープ (Counting Semaphore) */
        P(SEM_RENDER_COUNT);

        /* 2. クリティカルセクション: キューからの取り出し */
        P(SEM_RENDER_MUTEX);
        
        cmd = render_queue[render_tail];
        render_tail = (render_tail + 1) % RENDER_QUEUE_SIZE;
        
        V(SEM_RENDER_MUTEX);

        /* 3. 実際の描画処理 (ここが重い処理) */
        if (cmd.port_id == 0) target_fp = com0out;
        else target_fp = com1out;

        fprintf(target_fp, "%s", cmd.str);
        fflush(target_fp);
    }
}

/* ===========================================================================
 * ゲームロジック用ヘルパー関数
 * =========================================================================== */

/* ---------------------------------------------------------------------------
 * 関数名: queue_cell_draw
 * 概要  : 1セル分の描画コマンドを生成して送信する
 * --------------------------------------------------------------------------- */
void queue_cell_draw(int port, char cellVal) {
    if (cellVal == CELL_EMPTY) {
        send_draw_command(port, "%s・%s", BG_BLACK, ESC_RESET);
    } else if (cellVal == CELL_WALL) {
        send_draw_command(port, "%s%s■%s", BG_BLACK, COL_WALL, ESC_RESET);
    } else if (cellVal == CELL_GHOST) {
        send_draw_command(port, "%s%s□%s", BG_BLACK, COL_GRAY, ESC_RESET);
    } else if (cellVal >= 2 && cellVal <= 9) {
        /* ミノ (色付き) */
        send_draw_command(port, "%s%s■%s", BG_BLACK, minoColors[cellVal - 2], ESC_RESET);
    } else {
        send_draw_command(port, "??");
    }
}

/* ---------------------------------------------------------------------------
 * 関数名: isHit
 * 概要  : ミノの衝突判定を行う
 * 戻り値: 1=衝突あり，0=衝突なし
 * --------------------------------------------------------------------------- */
int isHit(TetrisGame *game, int _x, int _y, int _type, int _ang) {
    int i, j;
    for (i = 0; i < MINO_HEIGHT; i++) {
        for (j = 0; j < MINO_WIDTH; j++) {
            if (minoShapes[_type][_ang][i][j]) {
                int fy = _y + i, fx = _x + j;
                /* 境界チェック */
                if (fy < 0 || fy >= FIELD_HEIGHT || fx < 0 || fx >= FIELD_WIDTH) return 1;
                /* フィールドのブロックとの衝突チェック */
                if (game->field[fy][fx]) return 1;
            }
        }
    }
    return 0;
}

/* ---------------------------------------------------------------------------
 * 関数名: display
 * 概要  : ゲーム画面の描画処理 (差分更新方式)
 * 詳細  : 現在のフィールド，操作中ミノ，ゴーストを合成し，
 * 前回描画した内容と異なる部分のみエスケープシーケンスで更新する．
 * --------------------------------------------------------------------------- */
void display(TetrisGame *game) {
    int i, j;
    int changes = 0;
    
    int opponent_id = (game->port_id == 0) ? 1 : 0;
    TetrisGame *opponent = all_games[opponent_id];

    /* 相手接続時の強制更新フラグ */
    if (opponent != NULL && !game->opponent_was_connected) {
        memset(game->prevOpponentBuffer, -1, sizeof(game->prevOpponentBuffer));
    }
    game->opponent_was_connected = (opponent != NULL);

    /* --- 1. 描画バッファの構築 --- */
    /* フィールドをコピー */
    memcpy(game->displayBuffer, game->field, sizeof(game->field));

    /* ゴースト (落下位置予測) の描画 */
    if (game->minoType != MINO_GARBAGE) {
        int ghostY = game->minoY;
        /* 衝突するまでYを下げる */
        while (!isHit(game, game->minoX, ghostY + 1, game->minoType, game->minoAngle)) ghostY++;
        
        for (i = 0; i < MINO_HEIGHT; i++) {
            for (j = 0; j < MINO_WIDTH; j++) {
                if (minoShapes[game->minoType][game->minoAngle][i][j]) {
                    if (ghostY+i < FIELD_HEIGHT && game->minoX+j < FIELD_WIDTH &&
                        game->displayBuffer[ghostY+i][game->minoX+j] == CELL_EMPTY) {
                        game->displayBuffer[ghostY+i][game->minoX+j] = CELL_GHOST;
                    }
                }
            }
        }
    }

    /* 現在操作中のミノを描画 */
    for (i = 0; i < MINO_HEIGHT; i++) {
        for (j = 0; j < MINO_WIDTH; j++) {
            if (game->minoType != MINO_GARBAGE && minoShapes[game->minoType][game->minoAngle][i][j]) {
                if (game->minoY+i < FIELD_HEIGHT && game->minoX+j < FIELD_WIDTH) {
                    game->displayBuffer[game->minoY+i][game->minoX+j] = 2 + game->minoType;
                }
            }
        }
    }

    /* --- 2. 描画コマンドの発行 --- */
    
    /* ステータス表示 (ヘッダ部) */
    send_draw_command(game->port_id, "\x1b[1;1H"); /* Home */
    send_draw_command(game->port_id, "[YOU] SC:%-5d LN:%-3d ATK:%d", game->score, game->lines_cleared, game->pending_garbage);
    
    send_draw_command(game->port_id, "\x1b[1;%dH", OPPONENT_OFFSET_X);
    if (opponent) send_draw_command(game->port_id, "[RIVAL] SC:%-5d LN:%-3d", opponent->score, opponent->lines_cleared);
    else          send_draw_command(game->port_id, "[RIVAL] (Waiting...)    ");
    send_draw_command(game->port_id, "%s", ESC_CLR_LINE);

    send_draw_command(game->port_id, "\n--------------------------");
    if (opponent) send_draw_command(game->port_id, "\x1b[2;%dH--------------------------", OPPONENT_OFFSET_X);
    send_draw_command(game->port_id, "%s", ESC_CLR_LINE);

    /* フィールド部分の差分描画 */
    int base_y = 3;
    for (i = 0; i < FIELD_HEIGHT; i++) {
        /* 自分側の描画 */
        for (j = 0; j < FIELD_WIDTH; j++) {
            char val = game->displayBuffer[i][j];
            if (val != game->prevBuffer[i][j]) {
                /* カーソル移動して1文字更新 */
                send_draw_command(game->port_id, "\x1b[%d;%dH", base_y + i, j * 2 + 1);
                queue_cell_draw(game->port_id, val);
                game->prevBuffer[i][j] = val;
                changes++;
            }
        }
        /* 相手側の描画 (接続されている場合) */
        if (opponent) {
            for (j = 0; j < FIELD_WIDTH; j++) {
                char val = opponent->displayBuffer[i][j];
                if (val != game->prevOpponentBuffer[i][j]) {
                    send_draw_command(game->port_id, "\x1b[%d;%dH", base_y + i, OPPONENT_OFFSET_X + j * 2);
                    queue_cell_draw(game->port_id, val);
                    game->prevOpponentBuffer[i][j] = val;
                    changes++;
                }
            }
        }
    }
}

/* ---------------------------------------------------------------------------
 * 関数名: fillBag
 * 概要  : 7-Bag システムによるミノ順序の生成
 * --------------------------------------------------------------------------- */
void fillBag(TetrisGame *game) {
    int i, j, temp;
    for (i = 0; i < 7; i++) game->bag[i] = i;
    /* Fisher-Yates Shuffle */
    for (i = 6; i > 0; i--) {
        j = (tick + rand()) % (i + 1); 
        temp = game->bag[i]; game->bag[i] = game->bag[j]; game->bag[j] = temp;
    }
    game->bag_index = 0;
}

/* ---------------------------------------------------------------------------
 * 関数名: resetMino
 * 概要  : 新しいミノを出現させる
 * --------------------------------------------------------------------------- */
void resetMino(TetrisGame *game) {
    game->minoX = 5; game->minoY = 0;
    game->minoType = game->nextMinoType;
    game->minoAngle = (tick + rand()) % ANG_MAX;
    
    if (game->bag_index >= 7) fillBag(game);
    game->nextMinoType = game->bag[game->bag_index++];
}

/* ---------------------------------------------------------------------------
 * 関数名: processGarbage
 * 概要  : 相手から送られたお邪魔ラインを処理する
 * 戻り値: 1=処理の結果ゲームオーバー，0=継続可能
 * --------------------------------------------------------------------------- */
int processGarbage(TetrisGame *game) {
    int lines;
    
    /* === Mutex保護開始: 共有変数 pending_garbage へのアクセス === */
    P(SEM_GARBAGE);
    lines = game->pending_garbage;
    if (lines > 0) {
        /* 一度に処理するのは最大4ラインまで */
        if (lines > 4) { game->pending_garbage -= 4; lines = 4; }
        else { game->pending_garbage = 0; }
    }
    V(SEM_GARBAGE);
    /* === Mutex保護終了 === */

    if (lines <= 0) return 0;

    int i, j, k;
    /* 最上段にブロックがあるかチェック (あればゲームオーバー) */
    for (k = 0; k < lines; k++) {
        for (j = 1; j < FIELD_WIDTH - 1; j++) if (game->field[k][j]) return 1;
    }
    
    /* フィールド全体を上にシフト */
    for (i = 0; i < FIELD_HEIGHT - 1 - lines; i++) 
        memcpy(game->field[i], game->field[i + lines], FIELD_WIDTH);
        
    /* 最下段に穴あきラインを追加 */
    for (i = FIELD_HEIGHT - 1 - lines; i < FIELD_HEIGHT - 1; i++) {
        game->field[i][0] = game->field[i][FIELD_WIDTH-1] = 1; /* 壁 */
        for (j=1; j<FIELD_WIDTH-1; j++) game->field[i][j] = 2 + MINO_GARBAGE;
        /* ランダムな位置に穴を空ける */
        game->field[i][1 + (tick+rand()+i)%(FIELD_WIDTH-2)] = 0;
    }
    return 0;
}

/* ===========================================================================
 * メインループ (Game Task)
 * =========================================================================== */

/* ---------------------------------------------------------------------------
 * 関数名: wait_event
 * 概要  : イベント待機ループ
 * 詳細  : キー入力，タイマ満了，画面更新ポーリングを行う．
 * 入力がない場合は skipmt() でCPU時間を手放す．
 * --------------------------------------------------------------------------- */
Event wait_event(TetrisGame *game) {
    Event e = {EVT_NONE, 0};
    int c;
    int opp_id = (game->port_id == 0) ? 1 : 0;
    static int poll_cnt = 0;

    while (1) {
        /* 相手がゲームオーバーになったかチェック */
        if (all_games[opp_id] && all_games[opp_id]->is_gameover) { e.type = EVT_WIN; return e; }

        c = inbyte(game->port_id);
        if (c != -1) {
            /* --- キー入力処理 (エスケープシーケンス解析状態マシン) --- */
            if (game->seq_state == 0) {
                if (c == 0x1b) game->seq_state = 1; /* ESC */
                else if (c == 'q') { e.type = EVT_QUIT; return e; }
                else { e.type = EVT_KEY_INPUT; e.param = c; return e; }
            } else if (game->seq_state == 1) {
                if (c == '[') game->seq_state = 2; else game->seq_state = 0;
            } else if (game->seq_state == 2) {
                game->seq_state = 0;
                /* 矢印キーを WASD にマッピング */
                if (c=='A') e.param='w';      /* Up */
                else if (c=='B') e.param='s'; /* Down */
                else if (c=='C') e.param='d'; /* Right */
                else if (c=='D') e.param='a'; /* Left */
                
                if (e.param) { e.type = EVT_KEY_INPUT; return e; }
            }
        } else {
            /* --- 待機中の処理 --- */
            
            /* 落下タイマチェック */
            if (tick >= game->next_drop_time) { e.type = EVT_TIMER; return e; }
            
            /* 定期的な画面更新 (相手の状況反映のため) */
            poll_cnt++;
            if (poll_cnt >= DISPLAY_POLL_INTERVAL) {
                display(game); poll_cnt = 0;
                /* アニメーション中はイベントなしでも一旦リターンして処理を進める */
                if (game->state == GS_ANIMATING) { e.type = EVT_NONE; return e; }
            }
            
            /* 入力がないので他タスクへ譲る */
            skipmt();
        }
    }
}

/* ---------------------------------------------------------------------------
 * 関数名: wait_start_or_retry
 * 概要  : ゲーム開始前の同期待機
 * 詳細  : 自分のキー入力後，相手も準備完了 (世代ID一致) するまで待つ．
 * --------------------------------------------------------------------------- */
void wait_start_or_retry(TetrisGame *game, const char *msg) {
    int opp_id = (game->port_id == 0) ? 1 : 0;
    
    send_draw_command(game->port_id, ESC_CLS ESC_HOME "%s", msg);
    
    /* 自分のキー入力待ち */
    while(1) { if (inbyte(game->port_id) != -1) break; skipmt(); }
    
    /* 準備完了フラグ (世代) を更新 */
    srand((unsigned int)tick);
    game->sync_generation++;
    send_draw_command(game->port_id, ESC_CLR_LINE "\rWaiting for opponent...\n");
    
    /* 相手の世代が追いつくまで待機 */
    while(1) {
        if (all_games[opp_id]) {
            if (all_games[opp_id]->sync_generation == game->sync_generation) break;
        } else break; /* 相手がいない場合は即開始 */
        skipmt();
    }
}

/* ---------------------------------------------------------------------------
 * 関数名: run_tetris
 * 概要  : テトリスのメインロジック
 * --------------------------------------------------------------------------- */
void run_tetris(TetrisGame *game) {
    int i;
    /* 初期化 */
    game->score = 0; game->lines_cleared = 0; game->pending_garbage = 0;
    game->is_gameover = 0; game->state = GS_PLAYING;
    memset(game->prevBuffer, -1, sizeof(game->prevBuffer));
    
    /* 画面とフィールドの初期化 */
    send_draw_command(game->port_id, ESC_CLS ESC_HIDE_CUR);
    memset(game->field, 0, sizeof(game->field));
    for(i=0;i<FIELD_HEIGHT;i++) game->field[i][0] = game->field[i][FIELD_WIDTH-1] = 1; /* 左右壁 */
    for(i=0;i<FIELD_WIDTH;i++) game->field[FIELD_HEIGHT-1][i] = 1; /* 底 */

    /* 最初のミノ生成 */
    fillBag(game); game->nextMinoType = game->bag[game->bag_index++]; resetMino(game);
    display(game);
    game->next_drop_time = tick + DROP_INTERVAL;

    /* ゲームループ */
    while (1) {
        Event e = wait_event(game);

        /* --- アニメーション状態の処理 --- */
        if (game->state == GS_ANIMATING) {
            if (tick >= game->anim_start_tick + ANIMATION_DURATION) {
                /* アニメーション終了: 画面反転解除とライン消去確定 */
                send_draw_command(game->port_id, ESC_INVERT_OFF);
                game->lines_cleared += game->lines_to_clear;
                
                /* 攻撃判定 (2ライン以上で攻撃) */
                int attack = (game->lines_to_clear >= 2) ? (game->lines_to_clear == 4 ? 4 : game->lines_to_clear - 1) : 0;
                if (attack > 0) {
                    int opp_id = (game->port_id == 0) ? 1 : 0;
                    if (all_games[opp_id] && !all_games[opp_id]->is_gameover) {
                        /* === Mutex保護: 相手へ攻撃送信 === */
                        P(SEM_GARBAGE);
                        all_games[opp_id]->pending_garbage += attack;
                        V(SEM_GARBAGE);
                    }
                }
                
                game->state = GS_PLAYING;
                game->next_drop_time = tick + DROP_INTERVAL;
                goto PROCESS_GARBAGE; /* お邪魔処理へジャンプ */
            }
            continue;
        }

        /* --- 終了イベント処理 --- */
        if (e.type == EVT_WIN) {
            send_draw_command(game->port_id, ESC_CLS ESC_HOME "%sYOU WIN!%s", COL_RED, ESC_RESET);
            wait_start_or_retry(game, "Press Any Key to Retry..."); return;
        }
        if (e.type == EVT_QUIT) {
            send_draw_command(game->port_id, ESC_SHOW_CUR "Quit.\n");
            wait_start_or_retry(game, "Press Any Key to Retry..."); return;
        }

        /* --- キー入力イベント処理 --- */
        if (e.type == EVT_KEY_INPUT) {
            switch (e.param) {
                case 's': /* 落下 */
                    if(!isHit(game,game->minoX,game->minoY+1,game->minoType,game->minoAngle)) { 
                        game->minoY++; game->next_drop_time=tick+DROP_INTERVAL; 
                    } 
                    break;
                case 'a': /* 左移動 */
                    if(!isHit(game,game->minoX-1,game->minoY,game->minoType,game->minoAngle)) game->minoX--; 
                    break;
                case 'd': /* 右移動 */
                    if(!isHit(game,game->minoX+1,game->minoY,game->minoType,game->minoAngle)) game->minoX++; 
                    break;
                case ' ': /* 回転 */
                    {
                        int na = (game->minoAngle+1)%ANG_MAX;
                        if(!isHit(game,game->minoX,game->minoY,game->minoType,na)) game->minoAngle=na;
                        /* 壁蹴り補正 (簡易版) */
                        else if(!isHit(game,game->minoX+1,game->minoY,game->minoType,na)) { game->minoX++; game->minoAngle=na; }
                        else if(!isHit(game,game->minoX-1,game->minoY,game->minoType,na)) { game->minoX--; game->minoAngle=na; }
                    } 
                    break;
                case 'w': /* ハードドロップ */
                    while(!isHit(game,game->minoX,game->minoY+1,game->minoType,game->minoAngle)) game->minoY++; 
                    goto LOCK_MINO;
            }
            display(game);
        }

        /* --- タイマ(自然落下)イベント処理 --- */
        if (e.type == EVT_TIMER) {
            if (isHit(game, game->minoX, game->minoY + 1, game->minoType, game->minoAngle)) {
            LOCK_MINO:
                /* 固定処理 */
                for(i=0;i<4;i++) for(int j=0;j<4;j++) 
                    if(minoShapes[game->minoType][game->minoAngle][i][j]) 
                        game->field[game->minoY+i][game->minoX+j] = 2+game->minoType;
                
                /* ライン消去判定 */
                int lc = 0;
                for(i=0;i<FIELD_HEIGHT-1;i++) {
                    int f=1; for(int j=1;j<FIELD_WIDTH-1;j++) if(!game->field[i][j]) f=0;
                    if(f) {
                        /* ラインを消して詰める (実際のデータ更新) */
                        for(int k=i;k>0;k--) memcpy(game->field[k], game->field[k-1], FIELD_WIDTH);
                        lc++;
                    }
                }
                
                if (lc > 0) {
                    /* アニメーション開始 (画面反転) */
                    send_draw_command(game->port_id, "\a" ESC_INVERT_ON);
                    game->state = GS_ANIMATING; 
                    game->anim_start_tick = tick; 
                    game->lines_to_clear = lc;
                    continue; /* 次のループへ (アニメーション待ち) */
                }

            PROCESS_GARBAGE:
                /* お邪魔ライン処理 */
                if (processGarbage(game)) {
                    game->is_gameover = 1;
                    send_draw_command(game->port_id, ESC_CLS ESC_HOME "%sYOU LOSE%s", COL_BLUE, ESC_RESET);
                    wait_start_or_retry(game, "Press Any Key to Retry..."); return;
                }
                
                /* 次のミノへ */
                resetMino(game);
                
                /* 出現直後に衝突している場合はゲームオーバー */
                if (isHit(game, game->minoX, game->minoY, game->minoType, game->minoAngle)) {
                    game->is_gameover = 1;
                    send_draw_command(game->port_id, ESC_CLS ESC_HOME "%sYOU LOSE%s", COL_BLUE, ESC_RESET);
                    wait_start_or_retry(game, "Press Any Key to Retry..."); return;
                }
                game->next_drop_time = tick + DROP_INTERVAL;
            } else {
                /* 1段落下 */
                game->minoY++; game->next_drop_time = tick + DROP_INTERVAL;
            }
            display(game);
        }
    }
}

/* ===========================================================================
 * タスクエントリ & Main
 * =========================================================================== */

/* ---------------------------------------------------------------------------
 * 関数名: task1
 * 概要  : プレイヤー1 (Port 0) 用のタスク
 * --------------------------------------------------------------------------- */
void task1(void) {
    TetrisGame g; 
    g.port_id = 0; 
    g.sync_generation = 0; 
    all_games[0] = &g;
    
    wait_start_or_retry(&g, "TETRIS P1: Press Key");
    while(1) run_tetris(&g);
}

/* ---------------------------------------------------------------------------
 * 関数名: task2
 * 概要  : プレイヤー2 (Port 1) 用のタスク
 * --------------------------------------------------------------------------- */
void task2(void) {
    TetrisGame g; 
    g.port_id = 1; 
    g.sync_generation = 0; 
    all_games[1] = &g;
    
    wait_start_or_retry(&g, "TETRIS P2: Press Key");
    while(1) run_tetris(&g);
}

/* ---------------------------------------------------------------------------
 * 関数名: main
 * 概要  : システム初期化とタスク登録
 * --------------------------------------------------------------------------- */
int main(void) {
    init_kernel();
    
    /* セマフォ初期化 */
    semaphore[SEM_GARBAGE].count = 1;      /* Mutex: 初期値1 (利用可能) */
    semaphore[SEM_RENDER_MUTEX].count = 1; /* Mutex: 初期値1 (利用可能) */
    semaphore[SEM_RENDER_COUNT].count = 0; /* Counting: 初期値0 (データなし) */

    /* ファイルディスクリプタ設定 (Renderタスクが内部で使用) */
    com0in  = fdopen(0, "r"); com0out = fdopen(1, "w"); /* Port 0 (UART1) */
    com1in  = fdopen(4, "r"); com1out = fdopen(4, "w"); /* Port 1 (UART2) */
    
    /* タスク登録 (Game1, Game2, Renderer) */
    set_task(task1);
    set_task(task2);
    set_task(task_render);
    
    /* マルチタスクスケジューラ開始 (ここから戻ることはない) */
    begin_sch();
    return 0;
}