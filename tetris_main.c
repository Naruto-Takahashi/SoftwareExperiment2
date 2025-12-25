/* ===========================================================================
 * ファイル名: tetris_main.c
 * テーマ    : テーマ3 応用 (イベント駆動型マルチタスク・テトリス)
 * 作成日    : 2025/XX/XX
 *
 * [システム概要]
 * MC68VZ328用自作マルチタスクカーネル上で動作する、2人対戦型テトリスゲーム。
 * シリアルポート(UART)に接続されたVT100互換ターミナルに画面を描画する。
 *
 * [テーマ3 必須要件への対応と工夫点]
 * 1. イベント駆動型マルチタスク (協調的マルチタスク)
 * - 従来のターン制(Wait)ではなく、ノンブロッキング入力とイベントループを採用。
 * - 入力待ちやタイマー待機中は、自発的に `skipmt()` を呼び出してCPU時間を
 * 他タスクに譲ることで、システム全体の応答性を高めている。
 *
 * 2. セマフォによる排他制御 (共有資源の保護)
 * - 対戦相手への攻撃(お邪魔ライン送信)において、共有変数 `pending_garbage`
 * へのアクセスが発生する。
 * - データの不整合を防ぐため、バイナリセマフォ(SEM_GARBAGE_LOCK)を用いて
 * クリティカルセクションを保護している。
 *
 * [その他の特長]
 * - 7-Bagシステム: 公平性の高いミノ生成アルゴリズムの実装。
 * - 差分描画: 画面更新時に変更箇所のみを送信し、シリアル通信量を最小化。
 * - 対戦同期: ゲーム開始時とリトライ時に、両プレイヤーの準備完了を待機。
 * =========================================================================== */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "mtk_c.h"

/* ===========================================================================
 * 1. 外部依存定義 (Kernel / Driver / Library)
 * =========================================================================== */

/* --- 標準入出力ストリーム (csys68k.c 等で定義) --- */
extern FILE *com0in;
extern FILE *com0out;
extern FILE *com1in;
extern FILE *com1out;

/* --- カーネル制御関数 --- */
extern void init_kernel(void);
extern void set_task(void (*func)());
extern void begin_sch(void);

/* --- システムコール・ドライバ関数 --- */
extern int inbyte(int ch);      /* ノンブロッキング1文字入力 (戻り値 -1 で入力なし) */
extern void skipmt(void);       /* タスク切り替え要求 (CPU権の譲渡) */
extern void P(int sem_id);      /* P操作: セマフォ資源獲得 (Wait) */
extern void V(int sem_id);      /* V操作: セマフォ資源解放 (Signal) */

/* --- グローバル変数 --- */
extern volatile unsigned long tick;             /* システムタイマーティック */
extern SEMAPHORE_TYPE semaphore[NUMSEMAPHORE];  /* セマフォ管理配列 */


/* ===========================================================================
 * 2. 定数・マクロ定義
 * =========================================================================== */

/* --- セマフォID割り当て --- */
#define SEM_GARBAGE_LOCK 0      /* お邪魔ライン送受信の排他制御用 (Mutex) */

/* --- フィールド・ミノ設定 --- */
#define FIELD_WIDTH  12         /* フィールド幅 (壁含む) */
#define FIELD_HEIGHT 22         /* フィールド高さ (床含む) */
#define MINO_WIDTH   4          /* ミノデータ幅 */
#define MINO_HEIGHT  4          /* ミノデータ高さ */

/* --- 画面レイアウト --- */
#define OPPONENT_OFFSET_X 40    /* 対戦相手画面の表示Xオフセット */

/* --- ゲームバランス調整 --- */
#define DROP_INTERVAL      500  /* 自然落下の間隔 (tick単位) */
#define ANIMATION_DURATION 3    /* ライン消去演出の時間 (tick単位) */
#define COUNTDOWN_DELAY    1000 /* 開始カウントダウンの待ち時間 (tick単位) */

/* --- システム挙動設定 --- */
#define DISPLAY_POLL_INTERVAL 50 /* 描画更新処理の間引き間隔 (CPU負荷軽減) */

/* --- バッファ内部値 --- */
#define CELL_EMPTY  0           /* 空白セル */
#define CELL_WALL   1           /* 壁・床セル */
#define CELL_GHOST  10          /* ゴースト(落下予測)セル */

/* --- VT100 エスケープシーケンス (画面制御) --- */
#define ESC_CLS      "\x1b[2J"      /* 画面全消去 */
#define ESC_HOME     "\x1b[H"       /* カーソルをホーム位置へ */
#define ESC_RESET    "\x1b[0m"      /* 文字属性リセット */
#define ESC_HIDE_CUR "\x1b[?25l"    /* カーソル非表示 */
#define ESC_SHOW_CUR "\x1b[?25h"    /* カーソル表示 */
#define ESC_CLR_LINE "\x1b[K"       /* カーソル位置から行末まで消去 */
#define ESC_INVERT_ON  "\x1b[?5h"   /* 画面反転 (フラッシュ演出用) */
#define ESC_INVERT_OFF "\x1b[?5l"   /* 画面反転解除 */

/* --- TrueColor (24bit) 色定義 --- */
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
 * 3. データ構造体・型定義
 * =========================================================================== */

/* ゲームの進行状態 */
typedef enum {
    GS_PLAYING,     /* 通常プレイ中 (操作受付中) */
    GS_ANIMATING,   /* アニメーション演出中 (操作ブロック) */
    GS_GAMEOVER     /* ゲームオーバー状態 */
} GameState;

/* イベントの種類 (イベント駆動ロジック用) */
typedef enum {
    EVT_NONE,       /* イベントなし */
    EVT_KEY_INPUT,  /* キー入力発生 */
    EVT_TIMER,      /* 落下タイマー発火 */
    EVT_WIN,        /* 勝利判定 (相手が死亡) */
    EVT_QUIT        /* 中断要求 */
} EventType;

/* イベントデータ構造体 */
typedef struct {
    EventType type; /* イベント種別 */
    int param;      /* 付加情報 (キーコード等) */
} Event;

/* * [TetrisGame]
 * 1人のプレイヤーに必要な全ての状態を保持するコンテキスト構造体
 */
typedef struct {
    /* --- 入出力設定 --- */
    int port_id;        /* ポートID (0: UART1, 1: UART2) */
    FILE *fp_out;       /* 出力ストリーム */
    
    /* --- フィールド・描画バッファ --- */
    char field[FIELD_HEIGHT][FIELD_WIDTH];          /* 論理フィールド (確定ブロック) */
    char displayBuffer[FIELD_HEIGHT][FIELD_WIDTH];  /* 描画用合成バッファ */
    char prevBuffer[FIELD_HEIGHT][FIELD_WIDTH];     /* 前回描画時のバッファ (差分検出用) */
    
    /* --- 対戦相手同期用バッファ --- */
    char prevOpponentBuffer[FIELD_HEIGHT][FIELD_WIDTH]; 
    int opponent_was_connected;

    /* --- 状態管理 --- */
    GameState state;
    unsigned long anim_start_tick;  /* アニメーション開始時刻 */
    int lines_to_clear;             /* 消去予定ライン数 */

    /* --- 操作中のミノ --- */
    int minoType;   /* 種類 */
    int minoAngle;  /* 回転角度 (0-3) */
    int minoX;      /* X座標 */
    int minoY;      /* Y座標 */
    
    /* --- NEXTミノ / 生成管理 (7-Bag) --- */
    int nextMinoType;       
    int prevNextMinoType;   
    int bag[7];
    int bag_index;

    /* --- タイマー・入力制御 --- */
    unsigned long next_drop_time;   /* 次回落下予定時刻 */
    int seq_state;                  /* エスケープシーケンス解析ステート */
    
    /* --- スコア・統計 --- */
    int score;
    int lines_cleared;
    
    /* --- 共有資源 (★排他制御対象) --- */
    /* 他タスクから非同期に書き換えられるため volatile 修飾 */
    volatile int pending_garbage;   /* 未処理のお邪魔ライン数 */
    volatile int is_gameover;       /* ゲームオーバーフラグ */
    volatile int sync_generation;   /* 開始/リトライ同期用世代カウンタ */

} TetrisGame;

/* 全ゲームインスタンスへのポインタ (対戦相手参照用) */
TetrisGame *all_games[2] = {NULL, NULL};


/* ===========================================================================
 * 4. グローバルデータ (ミノ形状・色)
 * =========================================================================== */

/* ミノ種類ID */
enum { 
    MINO_TYPE_I, MINO_TYPE_O, MINO_TYPE_S, MINO_TYPE_Z, 
    MINO_TYPE_J, MINO_TYPE_L, MINO_TYPE_T, 
    MINO_TYPE_GARBAGE, 
    MINO_TYPE_MAX 
};

/* ミノ回転ID */
enum { 
    MINO_ANGLE_0, MINO_ANGLE_90, MINO_ANGLE_180, MINO_ANGLE_270, 
    MINO_ANGLE_MAX 
};

/* ミノの色テーブル */
const char* minoColors[MINO_TYPE_MAX] = { 
    COL_CYAN, COL_YELLOW, COL_GREEN, COL_RED, 
    COL_BLUE, COL_ORANGE, COL_PURPLE, COL_GRAY 
};

/* ミノ形状定義 (4x4x4方向, 0:なし 1:あり) */
char minoShapes[MINO_TYPE_MAX][MINO_ANGLE_MAX][MINO_HEIGHT][MINO_WIDTH] = {
    /* MINO_TYPE_I */
    {
        { {0, 1, 0, 0}, {0, 1, 0, 0}, {0, 1, 0, 0}, {0, 1, 0, 0} },
        { {0, 0, 0, 0}, {0, 0, 0, 0}, {1, 1, 1, 1}, {0, 0, 0, 0} },
        { {0, 0, 1, 0}, {0, 0, 1, 0}, {0, 0, 1, 0}, {0, 0, 1, 0} },
        { {0, 0, 0, 0}, {1, 1, 1, 1}, {0, 0, 0, 0}, {0, 0, 0, 0} }
    },
    /* MINO_TYPE_O */
    {
        { {0, 0, 0, 0}, {0, 1, 1, 0}, {0, 1, 1, 0}, {0, 0, 0, 0} },
        { {0, 0, 0, 0}, {0, 1, 1, 0}, {0, 1, 1, 0}, {0, 0, 0, 0} },
        { {0, 0, 0, 0}, {0, 1, 1, 0}, {0, 1, 1, 0}, {0, 0, 0, 0} },
        { {0, 0, 0, 0}, {0, 1, 1, 0}, {0, 1, 1, 0}, {0, 0, 0, 0} }
    },
    /* MINO_TYPE_S */
    {
        { {0, 0, 0, 0}, {0, 1, 1, 0}, {1, 1, 0, 0}, {0, 0, 0, 0} },
        { {0, 1, 0, 0}, {0, 1, 1, 0}, {0, 0, 1, 0}, {0, 0, 0, 0} },
        { {0, 0, 0, 0}, {0, 0, 1, 1}, {0, 1, 1, 0}, {0, 0, 0, 0} },
        { {0, 0, 0, 0}, {0, 1, 0, 0}, {0, 1, 1, 0}, {0, 0, 1, 0} }
    },
    /* MINO_TYPE_Z */
    {
        { {0, 0, 0, 0}, {1, 1, 0, 0}, {0, 1, 1, 0}, {0, 0, 0, 0} },
        { {0, 0, 1, 0}, {0, 1, 1, 0}, {0, 1, 0, 0}, {0, 0, 0, 0} },
        { {0, 0, 0, 0}, {0, 1, 1, 0}, {0, 0, 1, 1}, {0, 0, 0, 0} },
        { {0, 0, 0, 0}, {0, 0, 1, 0}, {0, 1, 1, 0}, {0, 1, 0, 0} }
    },
    /* MINO_TYPE_J */
    {
        { {0, 0, 1, 0}, {0, 0, 1, 0}, {0, 1, 1, 0}, {0, 0, 0, 0} },
        { {0, 0, 0, 0}, {0, 1, 0, 0}, {0, 1, 1, 1}, {0, 0, 0, 0} },
        { {0, 0, 0, 0}, {0, 1, 1, 0}, {0, 1, 0, 0}, {0, 1, 0, 0} },
        { {0, 0, 0, 0}, {1, 1, 1, 0}, {0, 0, 1, 0}, {0, 0, 0, 0} }
    },
    /* MINO_TYPE_L */
    {
        { {0, 1, 0, 0}, {0, 1, 0, 0}, {0, 1, 1, 0}, {0, 0, 0, 0} },
        { {0, 0, 0, 0}, {0, 1, 1, 1}, {0, 1, 0, 0}, {0, 0, 0, 0} },
        { {0, 0, 0, 0}, {0, 1, 1, 0}, {0, 0, 1, 0}, {0, 0, 1, 0} },
        { {0, 0, 0, 0}, {0, 0, 1, 0}, {1, 1, 1, 0}, {0, 0, 0, 0} }
    },
    /* MINO_TYPE_T */
    {
        { {0, 0, 0, 0}, {1, 1, 1, 0}, {0, 1, 0, 0}, {0, 0, 0, 0} },
        { {0, 0, 1, 0}, {0, 1, 1, 0}, {0, 0, 1, 0}, {0, 0, 0, 0} },
        { {0, 0, 0, 0}, {0, 0, 1, 0}, {0, 1, 1, 1}, {0, 0, 0, 0} },
        { {0, 0, 0, 0}, {0, 1, 0, 0}, {0, 1, 1, 0}, {0, 1, 0, 0} }
    },
    /* MINO_TYPE_GARBAGE (形状定義なし) */
    { {{0}},{{0}},{{0}},{{0}} }
};


/* ===========================================================================
 * 5. 関数プロトタイプ宣言
 * =========================================================================== */
int  isHit(TetrisGame *game, int _minoX, int _minoY, int _minoType, int _minoAngle);
void print_cell_content(FILE *fp, char cellVal);
void display(TetrisGame *game);
void perform_countdown(TetrisGame *game);
void wait_start(TetrisGame *game);
void wait_retry(TetrisGame *game);
void show_gameover_message(TetrisGame *game);
void show_victory_message(TetrisGame *game);
void run_tetris(TetrisGame *game);


/* ===========================================================================
 * 6. 描画・表示関連関数
 * =========================================================================== */

/* ---------------------------------------------------------------------------
 * 関数名: print_cell_content
 * 概要  : セル1つ分の描画内容を出力する
 * 引数  : fp      - 出力先ファイルポインタ
 * cellVal - セルの値 (0:空, 1:壁, 2~9:ミノ, 10:ゴースト)
 * --------------------------------------------------------------------------- */
void print_cell_content(FILE *fp, char cellVal) {
    if (cellVal == CELL_EMPTY) {
        fprintf(fp, "%s・%s", BG_BLACK, ESC_RESET);
    } else if (cellVal == CELL_WALL) {
        fprintf(fp, "%s%s■%s", BG_BLACK, COL_WALL, ESC_RESET);
    } else if (cellVal == CELL_GHOST) {
        fprintf(fp, "%s%s□%s", BG_BLACK, COL_GRAY, ESC_RESET);
    } else if (cellVal >= 2 && cellVal <= 9) {
        fprintf(fp, "%s%s■%s", BG_BLACK, minoColors[cellVal - 2], ESC_RESET);
    } else {
        fprintf(fp, "??");
    }
}

/* ---------------------------------------------------------------------------
 * 関数名: display
 * 概要  : 画面描画処理 (差分更新アルゴリズム採用)
 * 手順  : 
 * 1. 現在のフィールド、落下中ミノ、ゴーストを合成したバッファを作成。
 * 2. 前回描画したバッファ(prevBuffer)と比較し、変更点のみを出力する。
 * 3. 対戦相手が接続されている場合、相手の画面も同様に差分更新する。
 * --------------------------------------------------------------------------- */
void display(TetrisGame *game) {
    int i, j;
    int changes = 0;
    
    /* 対戦相手の取得 */
    int opponent_id = (game->port_id == 0) ? 1 : 0;
    TetrisGame *opponent = all_games[opponent_id];

    /* 相手接続時に全再描画フラグを立てる処理 */
    int opponent_connected = (opponent != NULL);
    if (opponent_connected && !game->opponent_was_connected) {
        memset(game->prevOpponentBuffer, -1, sizeof(game->prevOpponentBuffer));
    }
    game->opponent_was_connected = opponent_connected;

    /* --- [Step 1] 描画用バッファの作成 --- */
    
    /* フィールドの状態をベースにする */
    memcpy(game->displayBuffer, game->field, sizeof(game->field));

    /* ゴースト(落下予測位置)の計算と合成 */
    if (game->minoType != MINO_TYPE_GARBAGE) {
        int ghostY = game->minoY;
        while (!isHit(game, game->minoX, ghostY + 1, game->minoType, game->minoAngle)) {
            ghostY++;
        }
        
        for (i = 0; i < MINO_HEIGHT; i++) {
            for (j = 0; j < MINO_WIDTH; j++) {
                if (minoShapes[game->minoType][game->minoAngle][i][j]) {
                    /* フィールド範囲内で、かつ空セルの場合のみ描画 */
                    if (ghostY + i >= 0 && ghostY + i < FIELD_HEIGHT &&
                        game->minoX + j >= 0 && game->minoX + j < FIELD_WIDTH) {
                        if (game->displayBuffer[ghostY + i][game->minoX + j] == CELL_EMPTY) {
                            game->displayBuffer[ghostY + i][game->minoX + j] = CELL_GHOST;
                        }
                    }
                }
            }
        }
    }

    /* 落下中のミノを合成 */
    for (i = 0; i < MINO_HEIGHT; i++) {
        for (j = 0; j < MINO_WIDTH; j++) {
            if (game->minoType != MINO_TYPE_GARBAGE && 
                minoShapes[game->minoType][game->minoAngle][i][j]) {
                if (game->minoY + i >= 0 && game->minoY + i < FIELD_HEIGHT &&
                    game->minoX + j >= 0 && game->minoX + j < FIELD_WIDTH) {
                    game->displayBuffer[game->minoY + i][game->minoX + j] = 2 + game->minoType;
                }
            }
        }
    }

    /* --- [Step 2] ヘッダ情報の描画 --- */
    fprintf(game->fp_out, "\x1b[1;1H"); 
    fprintf(game->fp_out, "[YOU] SC:%-5d LN:%-3d ATK:%d", 
            game->score, game->lines_cleared, game->pending_garbage);
    
    if (opponent != NULL) {
        fprintf(game->fp_out, "\x1b[1;%dH", OPPONENT_OFFSET_X);
        fprintf(game->fp_out, "[RIVAL] SC:%-5d LN:%-3d", 
                opponent->score, opponent->lines_cleared);
    } else {
        fprintf(game->fp_out, "\x1b[1;%dH", OPPONENT_OFFSET_X);
        fprintf(game->fp_out, "[RIVAL] (Waiting...)    ");
    }
    fprintf(game->fp_out, "%s", ESC_CLR_LINE);

    fprintf(game->fp_out, "\n--------------------------");
    if (opponent != NULL) {
        fprintf(game->fp_out, "\x1b[2;%dH", OPPONENT_OFFSET_X);
        fprintf(game->fp_out, "--------------------------");
    }
    fprintf(game->fp_out, "%s", ESC_CLR_LINE);

    /* --- [Step 3] フィールドの差分描画 --- */
    int base_y = 3;

    for (i = 0; i < FIELD_HEIGHT; i++) {
        /* 左画面: 自分 */
        for (j = 0; j < FIELD_WIDTH; j++) {
            char myVal = game->displayBuffer[i][j];
            if (myVal != game->prevBuffer[i][j]) {
                fprintf(game->fp_out, "\x1b[%d;%dH", base_y + i, j * 2 + 1);
                print_cell_content(game->fp_out, myVal);
                game->prevBuffer[i][j] = myVal;
                changes++;
            }
        }

        /* 右画面: 対戦相手 */
        if (opponent != NULL) {
            for (j = 0; j < FIELD_WIDTH; j++) {
                char oppVal = opponent->displayBuffer[i][j];
                if (oppVal != game->prevOpponentBuffer[i][j]) {
                    fprintf(game->fp_out, "\x1b[%d;%dH", base_y + i, OPPONENT_OFFSET_X + j * 2);
                    print_cell_content(game->fp_out, oppVal);
                    game->prevOpponentBuffer[i][j] = oppVal;
                    changes++;
                }
            }
        }
    }
    
    /* 変更があった場合のみバッファをフラッシュ */
    if (changes > 0) fflush(game->fp_out);
}

/* ---------------------------------------------------------------------------
 * 関数名: perform_countdown
 * 概要  : ゲーム開始前のカウントダウン演出 (3 -> 2 -> 1 -> GO!)
 * 備考  : 待機ループ内で `skipmt()` を呼び出し、協調的動作を維持する。
 * --------------------------------------------------------------------------- */
void perform_countdown(TetrisGame *game) {
    const char *messages[] = {" 3 ", " 2 ", " 1 ", "GO!"};
    int i;
    int base_y = 3 + (FIELD_HEIGHT / 2) - 1;
    int base_x = 10; 

    for (i = 0; i < 4; i++) {
        fprintf(game->fp_out, "\x1b[%d;%dH%s%s   %s   %s", 
                base_y, base_x - 1, BG_BLACK, COL_YELLOW, messages[i], ESC_RESET);
        fflush(game->fp_out);

        if (i == 3) break;
        
        unsigned long target_tick = tick + COUNTDOWN_DELAY;
        while (tick < target_tick) {
            skipmt(); /* ★重要: 待機中はCPUを他タスクに譲る */
        }
    }

    /* 描画崩れ防止のためバッファをリセット */
    memset(game->prevBuffer, -1, sizeof(game->prevBuffer));
}


/* ===========================================================================
 * 7. イベント・入力制御関数
 * =========================================================================== */

/* ---------------------------------------------------------------------------
 * 関数名: wait_event
 * 概要  : イベントループ処理 (イベント駆動型マルチタスクの中核)
 * 引数  : game - ゲームコンテキスト
 * 戻り値: 発生したイベント構造体
 * 手順  :
 * 1. 対戦相手の勝利状態(GAME OVER)を確認。
 * 2. ノンブロッキング入力(inbyte)を確認。
 * - 入力あり: エスケープシーケンス解析 -> キーイベント発行。
 * - 入力なし:
 * a. タイマー時刻確認 -> 落下イベント発行。
 * b. 描画更新が必要なら実行。
 * c. ★重要: `skipmt()` を実行してCPU権を放棄(Yield)。
 * --------------------------------------------------------------------------- */
Event wait_event(TetrisGame *game) {
    Event e;
    e.type = EVT_NONE;
    int c;
    int opponent_id = (game->port_id == 0) ? 1 : 0;
    static int poll_counter = 0;

    while (1) {
        /* 1. 相手の勝敗チェック */
        if (all_games[opponent_id] != NULL && all_games[opponent_id]->is_gameover) {
            e.type = EVT_WIN;
            return e;
        }

        /* 2. 入力チェック (ノンブロッキング) */
        c = inbyte(game->port_id);
        if (c != -1) {
            /* キー入力あり: エスケープシーケンスのステートマシン解析 */
            if (game->seq_state == 0) {
                if (c == 0x1b) game->seq_state = 1;     /* ESC受信 */
                else if (c == 'q') { e.type = EVT_QUIT; return e; }
                else { e.type = EVT_KEY_INPUT; e.param = c; return e; }
            } 
            else if (game->seq_state == 1) {
                if (c == '[') game->seq_state = 2;      /* [ 受信 */
                else game->seq_state = 0;
            } 
            else if (game->seq_state == 2) {
                /* コマンド受信 */
                game->seq_state = 0;
                switch (c) {
                    case 'A': e.param = 'w'; break; /* 上 (ハードドロップ) */
                    case 'B': e.param = 's'; break; /* 下 (ソフトドロップ) */
                    case 'C': e.param = 'd'; break; /* 右 */
                    case 'D': e.param = 'a'; break; /* 左 */
                    default:  e.param = 0;   break;
                }
                if (e.param != 0) { e.type = EVT_KEY_INPUT; return e; }
            }
        } 
        else {
            /* 入力なし: タイマーチェック */
            if (tick >= game->next_drop_time) {
                e.type = EVT_TIMER;
                return e;
            }
            
            /* 定期的な画面更新 (ポーリング頻度調整) */
            poll_counter++;
            if (poll_counter >= DISPLAY_POLL_INTERVAL) {
                display(game); 
                poll_counter = 0;
                
                /* アニメーション中はイベントなしで継続 */
                if (game->state == GS_ANIMATING) {
                    e.type = EVT_NONE; 
                    return e; 
                }
            }
            
            /* ★重要: 処理がない時はタスク切り替え (協調的マルチタスク) */
            skipmt();
        }
    }
}


/* ===========================================================================
 * 8. ゲームロジック関数 (判定・計算)
 * =========================================================================== */

/* ---------------------------------------------------------------------------
 * 関数名: isHit
 * 概要  : 当たり判定を行う
 * 戻り値: 1 = 衝突あり, 0 = 衝突なし
 * --------------------------------------------------------------------------- */
int isHit(TetrisGame *game, int _minoX, int _minoY, int _minoType, int _minoAngle) {
    int i, j;
    for (i = 0; i < MINO_HEIGHT; i++) {
        for (j = 0; j < MINO_WIDTH; j++) {
            if (minoShapes[_minoType][_minoAngle][i][j]) {
                int fy = _minoY + i;
                int fx = _minoX + j;
                /* 壁・床・既存ブロックとの衝突を判定 */
                if (fy < 0 || fy >= FIELD_HEIGHT || fx < 0 || fx >= FIELD_WIDTH) return 1;
                if (game->field[fy][fx]) return 1;
            }
        }
    }
    return 0;
}

/* ---------------------------------------------------------------------------
 * 関数名: fillBag
 * 概要  : 7-Bag Systemによるミノ生成順序のシャッフル
 * --------------------------------------------------------------------------- */
void fillBag(TetrisGame *game) {
    int i, j, temp;
    for (i = 0; i < 7; i++) game->bag[i] = i;
    
    /* 現在のtick値をシードにしてシャッフル */
    for (i = 6; i > 0; i--) {
        j = (tick + rand()) % (i + 1); 
        temp = game->bag[i]; game->bag[i] = game->bag[j]; game->bag[j] = temp;
    }
    game->bag_index = 0;
}

/* ---------------------------------------------------------------------------
 * 関数名: resetMino
 * 概要  : NEXTミノを現在ミノに昇格させ、初期位置に配置する
 * --------------------------------------------------------------------------- */
void resetMino(TetrisGame *game) {
    game->minoX = 5;
    game->minoY = 0;
    game->minoType = game->nextMinoType;
    /* 初期角度もランダムに変化させる */
    game->minoAngle = (tick + rand()) % MINO_ANGLE_MAX;
    
    /* Bagが空になったら補充 */
    if (game->bag_index >= 7) fillBag(game);
    game->nextMinoType = game->bag[game->bag_index];
    game->bag_index++;
}

/* ---------------------------------------------------------------------------
 * 関数名: processGarbage
 * 概要  : お邪魔ライン処理 (共有資源の読み出しとフィールド操作)
 * ★重要: 共有変数 `pending_garbage` へのアクセスをセマフォで保護する。
 * --------------------------------------------------------------------------- */
int processGarbage(TetrisGame *game) {
    int lines;

    /* === [CRITICAL SECTION START] ====================================== */
    /* お邪魔ライン情報の読み出しとリセットは不可分操作である必要がある */
    P(SEM_GARBAGE_LOCK);
    
    lines = game->pending_garbage;
    if (lines > 0) {
        if (lines > 4) {
            game->pending_garbage -= 4; /* 一度に最大4ラインまで処理 */
            lines = 4;
        } else {
            game->pending_garbage = 0;  /* 全て処理済み */
        }
    }
    
    V(SEM_GARBAGE_LOCK);
    /* === [CRITICAL SECTION END] ======================================== */

    if (lines <= 0) return 0;

    int i, j, k;
    /* 天井判定 (最上部にブロックがあるとゲームオーバー) */
    for (k = 0; k < lines; k++) {
        for (j = 1; j < FIELD_WIDTH - 1; j++) {
            if (game->field[k][j] != 0) return 1; 
        }
    }
    
    /* フィールド全体を上にずらす */
    for (i = 0; i < FIELD_HEIGHT - 1 - lines; i++) {
        memcpy(game->field[i], game->field[i + lines], FIELD_WIDTH);
    }
    
    /* 下からお邪魔ライン(穴あき)を挿入 */
    for (i = FIELD_HEIGHT - 1 - lines; i < FIELD_HEIGHT - 1; i++) {
        game->field[i][0] = 1; 
        game->field[i][FIELD_WIDTH - 1] = 1; 
        for (j = 1; j < FIELD_WIDTH - 1; j++) {
            game->field[i][j] = 2 + MINO_TYPE_GARBAGE; 
        }
        int hole = 1 + (tick + rand() + i) % (FIELD_WIDTH - 2);
        game->field[i][hole] = 0;
    }
    return 0; 
}


/* ===========================================================================
 * 9. 同期・メッセージ表示関数
 * =========================================================================== */

/* ---------------------------------------------------------------------------
 * 関数名: wait_start
 * 概要  : ゲーム開始前の同期処理と乱数初期化
 * 手順  :
 * 1. 自身のキー入力待ち (人間が操作するまで待機)。
 * 2. 押された瞬間の `tick` をシードにして乱数を初期化 (ランダム性の確保)。
 * 3. 対戦相手の同期フラグを確認し、両者準備完了まで待機。
 * --------------------------------------------------------------------------- */
void wait_start(TetrisGame *game) {
    int opponent_id = (game->port_id == 0) ? 1 : 0;

    fprintf(game->fp_out, ESC_CLS ESC_HOME);
    fprintf(game->fp_out, "============================\n");
    fprintf(game->fp_out, "   TETRIS: 2-PLAYER BATTLE  \n");
    fprintf(game->fp_out, "============================\n");
    fprintf(game->fp_out, "\nPress Any Key to Start...\n");
    fflush(game->fp_out);

    /* 1. キー入力待ち */
    while (1) {
        int c = inbyte(game->port_id);
        if (c != -1) break; 
        skipmt();
    }

    /* 2. 乱数初期化 */
    srand((unsigned int)tick);

    /* 3. 開始同期 */
    game->sync_generation++;
    fprintf(game->fp_out, ESC_CLR_LINE);
    fprintf(game->fp_out, "\rWaiting for opponent...   \n");
    fflush(game->fp_out);

    while (1) {
        if (all_games[opponent_id] != NULL) {
            if (all_games[opponent_id]->sync_generation == game->sync_generation) break;
        } else {
            break; /* 相手未接続の場合は待たない */
        }
        skipmt();
    }
}

/* ---------------------------------------------------------------------------
 * 関数名: wait_retry
 * 概要  : リトライ待機処理 (手順はwait_startと同様)
 * --------------------------------------------------------------------------- */
void wait_retry(TetrisGame *game) {
    int opponent_id = (game->port_id == 0) ? 1 : 0;
    fprintf(game->fp_out, "\nPress 'R' to Retry...\n");
    fflush(game->fp_out);
    
    while (1) {
        int c = inbyte(game->port_id);
        if (c == 'r' || c == 'R') break; 
        skipmt();
    }
    
    srand((unsigned int)tick + rand());
    game->sync_generation++;
    fprintf(game->fp_out, ESC_CLR_LINE);
    fprintf(game->fp_out, "\rWaiting for opponent...   \n");
    fflush(game->fp_out);
    
    while (1) {
        if (all_games[opponent_id] != NULL) {
            if (all_games[opponent_id]->sync_generation == game->sync_generation) break;
        } else break;
        skipmt();
    }
}

void show_gameover_message(TetrisGame *game) {
    fprintf(game->fp_out, ESC_CLS ESC_HOME "%s", COL_BLUE);
    fprintf(game->fp_out, "============================\n");
    fprintf(game->fp_out, "         GAME OVER          \n");
    fprintf(game->fp_out, "          YOU LOSE          \n");
    fprintf(game->fp_out, "============================\n");
    fprintf(game->fp_out, "%sFinal Score: %d\n", ESC_RESET, game->score);
    fflush(game->fp_out);
}

void show_victory_message(TetrisGame *game) {
    fprintf(game->fp_out, ESC_CLS ESC_HOME "%s", COL_RED);
    fprintf(game->fp_out, "============================\n");
    fprintf(game->fp_out, "      CONGRATULATIONS!      \n");
    fprintf(game->fp_out, "          YOU WIN!          \n");
    fprintf(game->fp_out, "============================\n");
    fprintf(game->fp_out, "%sScore: %d\n", ESC_RESET, game->score);
    fflush(game->fp_out);
}


/* ===========================================================================
 * 10. メインゲームループ
 * =========================================================================== */

/* ---------------------------------------------------------------------------
 * 関数名: run_tetris
 * 概要  : ゲームのメインループ制御
 * 手順  :
 * 1. ゲーム状態の初期化 (変数、フィールド、ミノ)。
 * 2. 開始カウントダウン。
 * 3. 無限ループによるゲーム進行
 * - イベント取得 (wait_event)
 * - アニメーション処理 (完了時に攻撃送信処理)
 * - イベント処理 (勝利、終了、キー入力、タイマー落下)
 * --------------------------------------------------------------------------- */
void run_tetris(TetrisGame *game) {
    int i;
    
    /* --- [1. 初期化処理] --- */
    game->score = 0;
    game->lines_cleared = 0;
    game->pending_garbage = 0;
    game->is_gameover = 0;
    game->state = GS_PLAYING; 
    game->lines_to_clear = 0;
    
    game->seq_state = 0;
    game->opponent_was_connected = 0;
    game->prevNextMinoType = -1; 
    
    memset(game->prevBuffer, -1, sizeof(game->prevBuffer));
    memset(game->prevOpponentBuffer, -1, sizeof(game->prevOpponentBuffer));
    
    fprintf(game->fp_out, ESC_CLS ESC_HIDE_CUR); 
    
    /* フィールド枠作成 */
    memset(game->field, 0, sizeof(game->field));
    for (i = 0; i < FIELD_HEIGHT; i++) {
        game->field[i][0] = game->field[i][FIELD_WIDTH - 1] = 1; 
    }
    for (i = 0; i < FIELD_WIDTH; i++) {
        game->field[FIELD_HEIGHT - 1][i] = 1; 
    }

    fillBag(game); 
    game->nextMinoType = game->bag[game->bag_index++]; 
    resetMino(game); 

    display(game);
    
    /* --- [2. カウントダウン] --- */
    perform_countdown(game);

    display(game);
    game->next_drop_time = tick + DROP_INTERVAL;

    /* --- [3. ゲームループ] --- */
    while (1) {
        Event e = wait_event(game);

        /* --- アニメーション中の処理 --- */
        if (game->state == GS_ANIMATING) {
            if (tick >= game->anim_start_tick + ANIMATION_DURATION) {
                fprintf(game->fp_out, ESC_INVERT_OFF); 
                game->lines_cleared += game->lines_to_clear;
                
                /* 攻撃(お邪魔ライン)の計算 */
                int attack = 0;
                switch(game->lines_to_clear) {
                    case 2: attack = 1; break;
                    case 3: attack = 2; break;
                    case 4: attack = 4; break;
                }
                
                /* ★重要: セマフォによる排他制御 (攻撃送信)
                 * 対戦相手の `pending_garbage` 変数を書き換えるため、
                 * 競合を防ぐためにクリティカルセクションとする。
                 */
                if (attack > 0) {
                    int opponent_id = (game->port_id == 0) ? 1 : 0;
                    if (all_games[opponent_id] != NULL && !all_games[opponent_id]->is_gameover) {
                        
                        /* === [CRITICAL SECTION START] ================== */
                        P(SEM_GARBAGE_LOCK);
                        all_games[opponent_id]->pending_garbage += attack;
                        V(SEM_GARBAGE_LOCK);
                        /* === [CRITICAL SECTION END] ==================== */
                    }
                }
                
                /* スコア加算 */
                switch (game->lines_to_clear) {
                    case 1: game->score += 100; break;
                    case 2: game->score += 300; break;
                    case 3: game->score += 500; break;
                    case 4: game->score += 800; break;
                }

                game->state = GS_PLAYING;
                game->next_drop_time = tick + DROP_INTERVAL; 
                
                goto PROCESS_GARBAGE;
            }
            continue; 
        }

        /* --- イベント処理 --- */
        switch (e.type) {
            case EVT_WIN:
                show_victory_message(game);
                wait_retry(game); 
                return; 

            case EVT_QUIT:
                fprintf(game->fp_out, "%sQuit.\n", ESC_SHOW_CUR);
                wait_retry(game);
                return;

            case EVT_KEY_INPUT:
                /* キー操作 (移動・回転・ハードドロップ) */
                switch (e.param) {
                    case 's': /* Down */
                        if (!isHit(game, game->minoX, game->minoY + 1, game->minoType, game->minoAngle)) {
                            game->minoY++;
                            game->next_drop_time = tick + DROP_INTERVAL;
                        }
                        break;
                    case 'a': /* Left */
                        if (!isHit(game, game->minoX - 1, game->minoY, game->minoType, game->minoAngle)) game->minoX--;
                        break;
                    case 'd': /* Right */
                        if (!isHit(game, game->minoX + 1, game->minoY, game->minoType, game->minoAngle)) game->minoX++;
                        break;
                    case ' ': /* Rotate */
                        {
                            int newAngle = (game->minoAngle + 1) % MINO_ANGLE_MAX;
                            if (!isHit(game, game->minoX, game->minoY, game->minoType, newAngle)) {
                                game->minoAngle = newAngle;
                            }
                            else if (!isHit(game, game->minoX + 1, game->minoY, game->minoType, newAngle)) {
                                game->minoX++; game->minoAngle = newAngle;
                            }
                            else if (!isHit(game, game->minoX - 1, game->minoY, game->minoType, newAngle)) {
                                game->minoX--; game->minoAngle = newAngle;
                            }
                        }
                        break;
                    case 'w': /* Hard Drop */
                        while (!isHit(game, game->minoX, game->minoY + 1, game->minoType, game->minoAngle)) {
                            game->minoY++;
                            game->score += 2; 
                        }
                        display(game);
                        goto LOCK_PROCESS; 
                        break;
                }
                display(game);
                break;

            case EVT_TIMER:
                /* 自然落下処理 */
                if (isHit(game, game->minoX, game->minoY + 1, game->minoType, game->minoAngle)) {
                
                LOCK_PROCESS: 
                    /* 1. フィールドに固定 */
                    for (i = 0; i < MINO_HEIGHT; i++) {
                        for (int j = 0; j < MINO_WIDTH; j++) {
                            if (minoShapes[game->minoType][game->minoAngle][i][j]) {
                                if (game->minoY + i >= 0 && game->minoY + i < FIELD_HEIGHT &&
                                    game->minoX + j >= 0 && game->minoX + j < FIELD_WIDTH) {
                                    game->field[game->minoY + i][game->minoX + j] = 2 + game->minoType;
                                }
                            }
                        }
                    }
                    
                    /* 2. ライン消去判定 */
                    int lines_this_turn = 0;
                    for (i = 0; i < FIELD_HEIGHT - 1; i++) {
                        int lineFill = 1;
                        for (int j = 1; j < FIELD_WIDTH - 1; j++) {
                            if (game->field[i][j] == 0) { lineFill = 0; break; }
                        }
                        if (lineFill) {
                            int k;
                            for (k = i; k > 0; k--) memcpy(game->field[k], game->field[k - 1], FIELD_WIDTH);
                            memset(game->field[0], 0, FIELD_WIDTH);
                            game->field[0][0] = game->field[0][FIELD_WIDTH-1] = 1;
                            lines_this_turn++;
                        }
                    }

                    if (lines_this_turn > 0) {
                        fprintf(game->fp_out, "\a" ESC_INVERT_ON); 
                        fflush(game->fp_out);
                        game->state = GS_ANIMATING;
                        game->anim_start_tick = tick;
                        game->lines_to_clear = lines_this_turn;
                        break; 
                    }

                PROCESS_GARBAGE: 
                    /* 3. お邪魔ラインのせり上がり (排他制御あり) */
                    if (processGarbage(game)) {
                        game->is_gameover = 1;
                        fprintf(game->fp_out, "\a");
                        show_gameover_message(game);
                        wait_retry(game); 
                        return;
                    }
                    
                    /* 4. 次のミノ出現 */
                    resetMino(game);
                    
                    /* 出現即死亡判定 */
                    if (isHit(game, game->minoX, game->minoY, game->minoType, game->minoAngle)) {
                        game->is_gameover = 1;
                        fprintf(game->fp_out, "\a");
                        show_gameover_message(game);
                        wait_retry(game); 
                        return; 
                    }
                    game->next_drop_time = tick + DROP_INTERVAL;

                } else {
                    /* 落下継続 */
                    game->minoY++;
                    game->next_drop_time = tick + DROP_INTERVAL;
                }
                display(game);
                break;
            default:
                break;
        }
    }
}


/* ===========================================================================
 * 11. タスクエントリ & メイン関数
 * =========================================================================== */

/* ---------------------------------------------------------------------------
 * 関数名: task1
 * 概要  : プレイヤー1 (Port 0) 用のタスク
 * --------------------------------------------------------------------------- */
void task1(void) {
    TetrisGame game1;
    game1.port_id = 0;
    game1.fp_out = com0out;
    game1.sync_generation = 0;
    all_games[0] = &game1; 
    
    wait_start(&game1);
    while(1) { run_tetris(&game1); }
}

/* ---------------------------------------------------------------------------
 * 関数名: task2
 * 概要  : プレイヤー2 (Port 1) 用のタスク
 * --------------------------------------------------------------------------- */
void task2(void) {
    TetrisGame game2;
    game2.port_id = 1;
    game2.fp_out = com1out;
    game2.sync_generation = 0;
    all_games[1] = &game2;
    
    wait_start(&game2);
    while(1) { run_tetris(&game2); }
}

/* ---------------------------------------------------------------------------
 * 関数名: main
 * 概要  : システムエントリポイント
 * 手順  :
 * 1. カーネルの初期化。
 * 2. セマフォの初期化 (ID 0をMutexとして設定)。
 * 3. ファイルストリームの設定 (fdopen)。
 * 4. タスクの登録 (task1, task2)。
 * 5. マルチタスクスケジューリング開始。
 * --------------------------------------------------------------------------- */
int main(void) {
    init_kernel();
    
    /* * セマフォ初期化: Mutexとして利用するため初期値は1
     * (init_kernel内で全セマフォが1に初期化されている場合でも、
     * 明示的に行うことで意図を明確にする)
     */
    semaphore[SEM_GARBAGE_LOCK].count = 1;

    com0in  = fdopen(0, "r");
    com0out = fdopen(1, "w");
    com1in  = fdopen(4, "r");
    com1out = fdopen(4, "w");
    
    set_task(task1);
    set_task(task2);
    
    begin_sch();
    return 0;
}