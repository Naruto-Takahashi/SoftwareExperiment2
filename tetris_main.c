/* ===========================================================================
 * tetris_main.c
 * イベント駆動型マルチタスク・テトリス
 *
 * [概要]
 * MC68VZ328用マルチタスクカーネル上で動作する2人対戦テトリス．
 * シリアルポート(UART)経由でVT100互換ターミナルに画面を描画する．
 *
 * [主な機能]
 * - 7-Bagシステムによる公平なミノ生成
 * - 差分更新による描画最適化 (通信量削減)
 * - 対戦相手との同期処理 (開始時・リトライ時)
 * - ゴースト(落下予測)表示機能
 * - お邪魔ラインの送り合い機能
 * =========================================================================== */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "mtk_c.h"

/* ===========================================================================
 * 外部関数・変数の宣言 (Kernel / Hardware Dependent)
 * =========================================================================== */
extern FILE *com0in;
extern FILE *com0out;
extern FILE *com1in;
extern FILE *com1out;

extern void init_kernel(void);
extern void set_task(void (*func)());
extern void begin_sch(void);
extern int inbyte(int ch);      /* ノンブロッキング入力 (戻り値 -1 で入力なし) */
extern void skipmt(void);       /* タスク切り替え要求 (協調的マルチタスク) */

extern volatile unsigned long tick; 

/* ===========================================================================
 * 定数・マクロ定義
 * =========================================================================== */

/* --- フィールド設定 --- */
#define FIELD_WIDTH  12
#define FIELD_HEIGHT 22
#define MINO_WIDTH   4
#define MINO_HEIGHT  4

/* --- 画面レイアウト設定 --- */
#define OPPONENT_OFFSET_X 40    /* 相手画面の表示Xオフセット */

/* --- ゲームバランス設定 --- */
#define DROP_INTERVAL 500       /* 自然落下の間隔 (tick単位) */
#define ANIMATION_DURATION 3    /* ライン消去演出の時間 (tick単位) */
#define COUNTDOWN_DELAY 10000     /* カウントダウンの待ち時間 (tick単位) */

/* --- システム設定 --- */
#define DISPLAY_POLL_INTERVAL 50 /* 描画更新の間引き (CPU負荷軽減用) */

/* --- バッファ内の特殊値定義 --- */
#define CELL_EMPTY  0
#define CELL_WALL   1
#define CELL_GHOST  10          /* ゴースト表示用の内部値 */

/* --- VT100 エスケープシーケンス (制御コード) --- */
#define ESC_CLS      "\x1b[2J"      /* 画面クリア */
#define ESC_HOME     "\x1b[H"       /* カーソルをホームへ */
#define ESC_RESET    "\x1b[0m"      /* 属性リセット */
#define ESC_HIDE_CUR "\x1b[?25l"    /* カーソル非表示 */
#define ESC_SHOW_CUR "\x1b[?25h"    /* カーソル表示 */
#define ESC_CLR_LINE "\x1b[K"       /* 行末まで消去 */
#define ESC_INVERT_ON  "\x1b[?5h"   /* 画面反転 (フラッシュ) */
#define ESC_INVERT_OFF "\x1b[?5l"   /* 画面反転解除 */

/* --- TrueColor 定義 (24bitカラー) --- */
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

/* ゲームの進行状態 */
typedef enum {
    GS_PLAYING,     /* 通常プレイ中 (操作可能) */
    GS_ANIMATING,   /* ライン消去演出中 (操作不可・ウェイト) */
    GS_GAMEOVER     /* ゲームオーバー */
} GameState;

/* イベントの種類 */
typedef enum {
    EVT_NONE,       /* イベントなし */
    EVT_KEY_INPUT,  /* キー入力あり */
    EVT_TIMER,      /* 落下タイマー発火 */
    EVT_WIN,        /* 勝利 (相手がゲームオーバー) */
    EVT_QUIT,       /* 中断要求 */
    EVT_REFRESH     /* 画面更新 (予約) */
} EventType;

/* イベント構造体 */
typedef struct {
    EventType type;
    int param;      /* キーコードなどの付加情報 */
} Event;

/* テトリスのゲームコンテキスト構造体 */
typedef struct {
    /* 入出力設定 */
    int port_id;        /* ポート番号 (0 or 1) */
    FILE *fp_out;       /* 出力用ファイルポインタ */
    
    /* フィールドバッファ */
    char field[FIELD_HEIGHT][FIELD_WIDTH];          /* 論理フィールド (ロジック用) */
    char displayBuffer[FIELD_HEIGHT][FIELD_WIDTH];  /* 描画合成用バッファ */
    char prevBuffer[FIELD_HEIGHT][FIELD_WIDTH];     /* 前回描画時の状態 (差分検出用) */
    
    /* 対戦相手の画面同期用 */
    char prevOpponentBuffer[FIELD_HEIGHT][FIELD_WIDTH]; 
    int opponent_was_connected;

    /* 状態管理 */
    GameState state;
    unsigned long anim_start_tick;  /* アニメーション開始時刻 */
    int lines_to_clear;             /* 消去予定のライン数 */

    /* 操作中のミノ情報 */
    int minoType;   /* 種類 */
    int minoAngle;  /* 回転角度 (0-3) */
    int minoX;      /* X座標 */
    int minoY;      /* Y座標 */
    
    /* NEXTミノ情報 (描画機能は削除済みだがデータは保持) */
    int nextMinoType;       
    int prevNextMinoType;   

    /* ミノ生成 (7-Bag システム用) */
    int bag[7];
    int bag_index;

    /* タイマー管理 */
    unsigned long next_drop_time;
    
    /* 入力解析用ステート (エスケープシーケンス判定) */
    int seq_state;
    
    /* スコア・状況 */
    int score;
    int lines_cleared;
    
    /* 対戦・同期フラグ (volatile必須: 他タスクから書き換えられるため) */
    volatile int pending_garbage;   /* 相手から送られた未処理のお邪魔ライン */
    volatile int is_gameover;       /* 自身がゲームオーバーになったか */
    volatile int sync_generation;   /* ゲーム開始/リトライの同期用カウンタ */

} TetrisGame;

/* 全ゲームインスタンスへのポインタ (対戦相手参照用) */
TetrisGame *all_games[2] = {NULL, NULL};

/* ===========================================================================
 * グローバルデータ (ミノ形状・色定義)
 * =========================================================================== */

/* ミノの種類のID定義 */
enum { 
    MINO_TYPE_I, MINO_TYPE_O, MINO_TYPE_S, MINO_TYPE_Z, 
    MINO_TYPE_J, MINO_TYPE_L, MINO_TYPE_T, 
    MINO_TYPE_GARBAGE, 
    MINO_TYPE_MAX 
};

/* ミノの色定義 */
const char* minoColors[MINO_TYPE_MAX] = { 
    COL_CYAN, COL_YELLOW, COL_GREEN, COL_RED, 
    COL_BLUE, COL_ORANGE, COL_PURPLE, COL_GRAY 
};

/* 回転角度のID定義 */
enum { 
    MINO_ANGLE_0, MINO_ANGLE_90, MINO_ANGLE_180, MINO_ANGLE_270, 
    MINO_ANGLE_MAX 
};

/* ミノ形状データ (0/1) */
char minoShapes[MINO_TYPE_MAX][MINO_ANGLE_MAX][MINO_HEIGHT][MINO_WIDTH] = {
    /* MINO_TYPE_I */
    {
        {
            {0, 1, 0, 0},
            {0, 1, 0, 0},
            {0, 1, 0, 0},
            {0, 1, 0, 0}
        },
        {
            {0, 0, 0, 0},
            {0, 0, 0, 0},
            {1, 1, 1, 1},
            {0, 0, 0, 0}
        },
        {
            {0, 0, 1, 0},
            {0, 0, 1, 0},
            {0, 0, 1, 0},
            {0, 0, 1, 0}
        },
        {
            {0, 0, 0, 0},
            {1, 1, 1, 1},
            {0, 0, 0, 0},
            {0, 0, 0, 0}
        }
    },
    /* MINO_TYPE_O */
    {
        {
            {0, 0, 0, 0},
            {0, 1, 1, 0},
            {0, 1, 1, 0},
            {0, 0, 0, 0}
        },
        {
            {0, 0, 0, 0},
            {0, 1, 1, 0},
            {0, 1, 1, 0},
            {0, 0, 0, 0}
        },
        {
            {0, 0, 0, 0},
            {0, 1, 1, 0},
            {0, 1, 1, 0},
            {0, 0, 0, 0}
        },
        {
            {0, 0, 0, 0},
            {0, 1, 1, 0},
            {0, 1, 1, 0},
            {0, 0, 0, 0}
        }
    },
    /* MINO_TYPE_S */
    {
        {
            {0, 0, 0, 0},
            {0, 1, 1, 0},
            {1, 1, 0, 0},
            {0, 0, 0, 0}
        },
        {
            {0, 1, 0, 0},
            {0, 1, 1, 0},
            {0, 0, 1, 0},
            {0, 0, 0, 0}
        },
        {
            {0, 0, 0, 0},
            {0, 0, 1, 1},
            {0, 1, 1, 0},
            {0, 0, 0, 0}
        },
        {
            {0, 0, 0, 0},
            {0, 1, 0, 0},
            {0, 1, 1, 0},
            {0, 0, 1, 0}
        }
    },
    /* MINO_TYPE_Z */
    {
        {
            {0, 0, 0, 0},
            {1, 1, 0, 0},
            {0, 1, 1, 0},
            {0, 0, 0, 0}
        },
        {
            {0, 0, 1, 0},
            {0, 1, 1, 0},
            {0, 1, 0, 0},
            {0, 0, 0, 0}
        },
        {
            {0, 0, 0, 0},
            {0, 1, 1, 0},
            {0, 0, 1, 1},
            {0, 0, 0, 0}
        },
        {
            {0, 0, 0, 0},
            {0, 0, 1, 0},
            {0, 1, 1, 0},
            {0, 1, 0, 0}
        }
    },
    /* MINO_TYPE_J */
    {
        {
            {0, 0, 1, 0},
            {0, 0, 1, 0},
            {0, 1, 1, 0},
            {0, 0, 0, 0}
        },
        {
            {0, 0, 0, 0},
            {0, 1, 0, 0},
            {0, 1, 1, 1},
            {0, 0, 0, 0}
        },
        {
            {0, 0, 0, 0},
            {0, 1, 1, 0},
            {0, 1, 0, 0},
            {0, 1, 0, 0}
        },
        {
            {0, 0, 0, 0},
            {1, 1, 1, 0},
            {0, 0, 1, 0},
            {0, 0, 0, 0}
        }
    },
    /* MINO_TYPE_L */
    {
        {
            {0, 1, 0, 0},
            {0, 1, 0, 0},
            {0, 1, 1, 0},
            {0, 0, 0, 0}
        },
        {
            {0, 0, 0, 0},
            {0, 1, 1, 1},
            {0, 1, 0, 0},
            {0, 0, 0, 0}
        },
        {
            {0, 0, 0, 0},
            {0, 1, 1, 0},
            {0, 0, 1, 0},
            {0, 0, 1, 0}
        },
        {
            {0, 0, 0, 0},
            {0, 0, 1, 0},
            {1, 1, 1, 0},
            {0, 0, 0, 0}
        }
    },
    /* MINO_TYPE_T */
    {
        {
            {0, 0, 0, 0},
            {1, 1, 1, 0},
            {0, 1, 0, 0},
            {0, 0, 0, 0}
        },
        {
            {0, 0, 1, 0},
            {0, 1, 1, 0},
            {0, 0, 1, 0},
            {0, 0, 0, 0}
        },
        {
            {0, 0, 0, 0},
            {0, 0, 1, 0},
            {0, 1, 1, 1},
            {0, 0, 0, 0}
        },
        {
            {0, 0, 0, 0},
            {0, 1, 0, 0},
            {0, 1, 1, 0},
            {0, 1, 0, 0}
        }
    },
    /* Garbage */
    { {{0}},{{0}},{{0}},{{0}} }
};

/* ===========================================================================
 * プロトタイプ宣言
 * =========================================================================== */
int isHit(TetrisGame *game, int _minoX, int _minoY, int _minoType, int _minoAngle);
void print_cell_content(FILE *fp, char cellVal);
void display(TetrisGame *game);

/* ===========================================================================
 * 描画関連 関数群
 * =========================================================================== */

/*
 * [print_cell_content]
 * 1セル分の内容を出力する．
 * note: BG_BLACK (\x1b[40m) を毎回指定し，背景描画の整合性を保つ．
 */
void print_cell_content(FILE *fp, char cellVal) {
    if (cellVal == CELL_EMPTY) {
        /* 空白: ドット表示 */
        fprintf(fp, "%s・%s", BG_BLACK, ESC_RESET);
    } else if (cellVal == CELL_WALL) {
        /* 壁: 白色ブロック */
        fprintf(fp, "%s%s■%s", BG_BLACK, COL_WALL, ESC_RESET);
    } else if (cellVal == CELL_GHOST) {
        /* ゴースト: 中抜きの四角 */
        fprintf(fp, "%s%s□%s", BG_BLACK, COL_GRAY, ESC_RESET);
    } else if (cellVal >= 2 && cellVal <= 9) {
        /* ミノ: 各色ブロック (背景は黒) */
        fprintf(fp, "%s%s■%s", BG_BLACK, minoColors[cellVal - 2], ESC_RESET);
    } else {
        /* 未定義値のエラー表示 */
        fprintf(fp, "??");
    }
}

/*
 * [display]
 * メインの画面描画関数．
 * 概要: 現在のフィールド，ゴースト，落下中のミノを合成し，
 * 前回のフレームとの差分のみを出力することで通信量を削減する．
 */
void display(TetrisGame *game) {
    int i, j;
    int changes = 0;
    
    /* 対戦相手の特定 */
    int opponent_id = (game->port_id == 0) ? 1 : 0;
    TetrisGame *opponent = all_games[opponent_id];

    /* 相手が接続されたら，相手画面の強制再描画フラグを立てる */
    int opponent_connected = (opponent != NULL);
    if (opponent_connected && !game->opponent_was_connected) {
        memset(game->prevOpponentBuffer, -1, sizeof(game->prevOpponentBuffer));
    }
    game->opponent_was_connected = opponent_connected;

    /* -----------------------------------------------
     * 1. 描画用バッファの作成
     * ----------------------------------------------- */
    
    /* 1-1. 現在のフィールド状態をコピー */
    memcpy(game->displayBuffer, game->field, sizeof(game->field));

    /* 1-2. ゴースト(落下予測位置)の計算と描画 */
    if (game->minoType != MINO_TYPE_GARBAGE) {
        int ghostY = game->minoY;
        while (!isHit(game, game->minoX, ghostY + 1, game->minoType, game->minoAngle)) {
            ghostY++;
        }
        
        for (i = 0; i < MINO_HEIGHT; i++) {
            for (j = 0; j < MINO_WIDTH; j++) {
                if (minoShapes[game->minoType][game->minoAngle][i][j]) {
                    if (ghostY + i >= 0 && ghostY + i < FIELD_HEIGHT &&
                        game->minoX + j >= 0 && game->minoX + j < FIELD_WIDTH) {
                        /* 空の場合のみ書き込む(ミノ本体の上書き防止) */
                        if (game->displayBuffer[ghostY + i][game->minoX + j] == CELL_EMPTY) {
                            game->displayBuffer[ghostY + i][game->minoX + j] = CELL_GHOST;
                        }
                    }
                }
            }
        }
    }

    /* 1-3. 現在落下中のミノを描画 */
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

    /* -----------------------------------------------
     * 2. ヘッダ情報(スコア等)の描画
     * ----------------------------------------------- */
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

    /* -----------------------------------------------
     * 3. フィールド描画 (差分更新アルゴリズム)
     * ----------------------------------------------- */
    int base_y = 3;

    for (i = 0; i < FIELD_HEIGHT; i++) {
        /* --- 左側: 自分のフィールド --- */
        for (j = 0; j < FIELD_WIDTH; j++) {
            char myVal = game->displayBuffer[i][j];
            if (myVal != game->prevBuffer[i][j]) {
                fprintf(game->fp_out, "\x1b[%d;%dH", base_y + i, j * 2 + 1);
                print_cell_content(game->fp_out, myVal);
                game->prevBuffer[i][j] = myVal;
                changes++;
            }
        }

        /* --- 右側: 相手のフィールド --- */
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
    
    /* 描画に変更があった場合のみフラッシュ */
    if (changes > 0) fflush(game->fp_out);
}

/*
 * [perform_countdown]
 * 開始時のカウントダウン (3 -> 2 -> 1 -> GO!)
 * note: "GO!" 表示後はウェイトを入れず即座に入力受付を開始する．
 */
void perform_countdown(TetrisGame *game) {
    const char *messages[] = {" 3 ", " 2 ", " 1 ", "GO!"};
    int i;
    int base_y = 3 + (FIELD_HEIGHT / 2) - 1;
    int base_x = 10; /* 常に左側(YOU)に表示 */

    for (i = 0; i < 4; i++) {
        fprintf(game->fp_out, "\x1b[%d;%dH%s%s   %s   %s", 
                base_y, base_x - 1, BG_BLACK, COL_YELLOW, messages[i], ESC_RESET);
        fflush(game->fp_out);

        /* GO! (i==3) の場合，即座にループを抜ける */
        if (i == 3) {
             break;
        }
        
        unsigned long target_tick = tick + COUNTDOWN_DELAY;
        while (tick < target_tick) {
            skipmt();
        }
    }

    /* バッファをリセット（次のdisplayで文字を消去させるため） */
    memset(game->prevBuffer, -1, sizeof(game->prevBuffer));
}

/* ===========================================================================
 * イベント制御・入力処理
 * =========================================================================== */

/*
 * [wait_event]
 * イベント待機・処理ループ．
 * 機能:
 * - 相手の勝敗チェック
 * - シリアル入力のポーリングとエスケープシーケンス解析
 * - タイマーによる自然落下
 * - アニメーション中の描画更新
 */
Event wait_event(TetrisGame *game) {
    Event e;
    e.type = EVT_NONE;
    int c;
    int opponent_id = (game->port_id == 0) ? 1 : 0;
    static int poll_counter = 0;

    while (1) {
        /* 相手が負けたかチェック */
        if (all_games[opponent_id] != NULL && all_games[opponent_id]->is_gameover) {
            e.type = EVT_WIN;
            return e;
        }

        /* ノンブロッキング入力取得 */
        c = inbyte(game->port_id);
        if (c != -1) {
            /* VT100 エスケープシーケンス解析ステートマシン */
            if (game->seq_state == 0) {
                if (c == 0x1b) game->seq_state = 1;
                else if (c == 'q') { e.type = EVT_QUIT; return e; }
                else { e.type = EVT_KEY_INPUT; e.param = c; return e; }
            } 
            else if (game->seq_state == 1) {
                if (c == '[') game->seq_state = 2;
                else game->seq_state = 0;
            } 
            else if (game->seq_state == 2) {
                game->seq_state = 0;
                switch (c) {
                    case 'A': e.param = 'w'; break; /* 上矢印 */
                    case 'B': e.param = 's'; break; /* 下矢印 */
                    case 'C': e.param = 'd'; break; /* 右矢印 */
                    case 'D': e.param = 'a'; break; /* 左矢印 */
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
            
            /* 描画更新 (負荷軽減のため間引き) */
            poll_counter++;
            if (poll_counter >= DISPLAY_POLL_INTERVAL) {
                display(game); 
                poll_counter = 0;
                /* アニメーション中は入力を待たずにループを回す */
                if (game->state == GS_ANIMATING) {
                    e.type = EVT_NONE; 
                    return e; 
                }
            }
            skipmt(); /* 他タスクへ制御を譲る */
        }
    }
}

/* ===========================================================================
 * ゲームロジック (判定・データ操作)
 * =========================================================================== */

/*
 * [isHit]
 * 当たり判定を行う．壁，床，既存ブロックとの衝突を検知する．
 */
int isHit(TetrisGame *game, int _minoX, int _minoY, int _minoType, int _minoAngle) {
    int i, j;
    for (i = 0; i < MINO_HEIGHT; i++) {
        for (j = 0; j < MINO_WIDTH; j++) {
            if (minoShapes[_minoType][_minoAngle][i][j]) {
                int fy = _minoY + i;
                int fx = _minoX + j;
                if (fy < 0 || fy >= FIELD_HEIGHT || fx < 0 || fx >= FIELD_WIDTH) return 1;
                if (game->field[fy][fx]) return 1;
            }
        }
    }
    return 0;
}

/*
 * [fillBag]
 * 7-Bag System: 7種類のミノをランダムに並び替えて袋(bag)に詰める．
 */
void fillBag(TetrisGame *game) {
    int i, j, temp;
    for (i = 0; i < 7; i++) game->bag[i] = i;
    for (i = 6; i > 0; i--) {
        j = (tick + rand()) % (i + 1); 
        temp = game->bag[i]; game->bag[i] = game->bag[j]; game->bag[j] = temp;
    }
    game->bag_index = 0;
}

/*
 * [resetMino]
 * 次のミノをセットし，初期位置に配置する．
 */
void resetMino(TetrisGame *game) {
    game->minoX = 5;
    game->minoY = 0;
    game->minoType = game->nextMinoType;
    game->minoAngle = (tick + rand()) % MINO_ANGLE_MAX;
    
    /* Bagが空になったら補充 */
    if (game->bag_index >= 7) fillBag(game);
    game->nextMinoType = game->bag[game->bag_index];
    game->bag_index++;
}

/*
 * [processGarbage]
 * お邪魔ライン(Garbage)のせり上がり処理．
 * 戻り値: 1ならゲームオーバー(天井に到達)，0なら成功．
 */
int processGarbage(TetrisGame *game) {
    int lines = game->pending_garbage;
    if (lines <= 0) return 0;
    if (lines > 4) lines = 4; /* 一度の処理上限 */
    game->pending_garbage -= lines;

    int i, j, k;
    /* 天井判定: せり上がる分だけ上部にブロックがあると死ぬ */
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
        game->field[i][0] = 1; /* 左壁 */
        game->field[i][FIELD_WIDTH - 1] = 1; /* 右壁 */
        for (j = 1; j < FIELD_WIDTH - 1; j++) {
            game->field[i][j] = 2 + MINO_TYPE_GARBAGE; 
        }
        /* ランダムな穴を開ける */
        int hole = 1 + (tick + rand() + i) % (FIELD_WIDTH - 2);
        game->field[i][hole] = 0;
    }
    return 0; 
}

/* ===========================================================================
 * 同期・メッセージ表示関連
 * =========================================================================== */

void wait_start(TetrisGame *game) {
    int opponent_id = (game->port_id == 0) ? 1 : 0;

    fprintf(game->fp_out, ESC_CLS ESC_HOME);
    fprintf(game->fp_out, "============================\n");
    fprintf(game->fp_out, "   TETRIS: 2-PLAYER BATTLE  \n");
    fprintf(game->fp_out, "============================\n");
    fprintf(game->fp_out, "\nPress Any Key to Start...\n");
    fflush(game->fp_out);

    /* キー入力待ち */
    while (1) {
        int c = inbyte(game->port_id);
        if (c != -1) break; 
        skipmt();
    }

    srand((unsigned int)tick);
    game->sync_generation++;
    
    fprintf(game->fp_out, ESC_CLR_LINE);
    fprintf(game->fp_out, "\rWaiting for opponent...   \n");
    fflush(game->fp_out);

    /* 相手との同期待ち */
    while (1) {
        if (all_games[opponent_id] != NULL) {
            if (all_games[opponent_id]->sync_generation == game->sync_generation) break;
        } else break; /* 相手未接続なら進む仕様 */
        skipmt();
    }
}

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
 * メインゲームループ
 * =========================================================================== */
void run_tetris(TetrisGame *game) {
    int i;
    
    /* --- ゲームパラメータ初期化 --- */
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
    
    /* 画面クリア */
    fprintf(game->fp_out, ESC_CLS ESC_HIDE_CUR); 
    
    /* フィールド初期化 (枠作成) */
    memset(game->field, 0, sizeof(game->field));
    for (i = 0; i < FIELD_HEIGHT; i++) {
        game->field[i][0] = game->field[i][FIELD_WIDTH - 1] = 1; 
    }
    for (i = 0; i < FIELD_WIDTH; i++) {
        game->field[FIELD_HEIGHT - 1][i] = 1; 
    }

    /* 最初のミノ生成 */
    fillBag(game); 
    game->nextMinoType = game->bag[game->bag_index++]; 
    resetMino(game); 

    /* 初期画面描画とカウントダウン */
    display(game);
    perform_countdown(game);

    /* ゲーム開始直後のラグ対策: タイマーリセット */
    display(game);
    game->next_drop_time = tick + DROP_INTERVAL;

    /* --- メインループ --- */
    while (1) {
        Event e = wait_event(game);

        /* アニメーション状態の処理 */
        if (game->state == GS_ANIMATING) {
            if (tick >= game->anim_start_tick + ANIMATION_DURATION) {
                fprintf(game->fp_out, ESC_INVERT_OFF); 
                game->lines_cleared += game->lines_to_clear;
                
                /* 攻撃(お邪魔ライン)の計算 */
                int attack = 0;
                switch(game->lines_to_clear) {
                    case 1: break;              /* 1列: 0 */
                    case 2: attack = 1; break;  /* 2列: 1 */
                    case 3: attack = 2; break;  /* 3列: 2 */
                    case 4: attack = 4; break;  /* 4列: 4 */
                }
                
                if (attack > 0) {
                    int opponent_id = (game->port_id == 0) ? 1 : 0;
                    if (all_games[opponent_id] != NULL && !all_games[opponent_id]->is_gameover) {
                        all_games[opponent_id]->pending_garbage += attack;
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
                
                /* アニメーション終了後にガベージ処理へ移行 */
                goto PROCESS_GARBAGE;
            }
            continue; 
        }

        /* 通常プレイ時のイベント処理 */
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
                    case ' ': /* Rotate with Wall Kick (簡易スーパーローテーション) */
                        {
                            int newAngle = (game->minoAngle + 1) % MINO_ANGLE_MAX;
                            
                            /* 優先順位1: 通常回転 (その場で回れるならそのまま) */
                            if (!isHit(game, game->minoX, game->minoY, game->minoType, newAngle)) {
                                game->minoAngle = newAngle;
                            }
                            /* 優先順位2: 右に1ズレて回転 (左壁際対策) */
                            else if (!isHit(game, game->minoX + 1, game->minoY, game->minoType, newAngle)) {
                                game->minoX++;
                                game->minoAngle = newAngle;
                            }
                            /* 優先順位3: 左に1ズレて回転 (右壁際対策) */
                            else if (!isHit(game, game->minoX - 1, game->minoY, game->minoType, newAngle)) {
                                game->minoX--;
                                game->minoAngle = newAngle;
                            }
                            /* 優先順位4: I型用 右に2ズレ (I型は長いため1マスでは足りない場合がある) */
                            else if (game->minoType == MINO_TYPE_I && 
                                     !isHit(game, game->minoX + 2, game->minoY, game->minoType, newAngle)) {
                                game->minoX += 2;
                                game->minoAngle = newAngle;
                            }
                            /* 優先順位5: I型用 左に2ズレ */
                            else if (game->minoType == MINO_TYPE_I && 
                                     !isHit(game, game->minoX - 2, game->minoY, game->minoType, newAngle)) {
                                game->minoX -= 2;
                                game->minoAngle = newAngle;
                            }
                            /* どのパターンでも回れなければ回転しない */
                        }
                        break;
                    case 'w': /* Hard Drop */
                        while (!isHit(game, game->minoX, game->minoY + 1, game->minoType, game->minoAngle)) {
                            game->minoY++;
                            game->score += 2; 
                        }
                        display(game);
                        goto LOCK_PROCESS; /* 即座に固定処理へ */
                        break;
                }
                display(game);
                break;

            case EVT_TIMER:
                /* 自然落下処理 */
                if (isHit(game, game->minoX, game->minoY + 1, game->minoType, game->minoAngle)) {
                
                LOCK_PROCESS: /* ミノ固定処理ラベル */
                    /* フィールドに固定 */
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
                    
                    /* ライン消去判定 */
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
                        /* 消去演出開始 */
                        fprintf(game->fp_out, "\a" ESC_INVERT_ON); 
                        fflush(game->fp_out);
                        game->state = GS_ANIMATING;
                        game->anim_start_tick = tick;
                        game->lines_to_clear = lines_this_turn;
                        break; /* イベントループに戻りアニメーション待機 */
                    }

                PROCESS_GARBAGE: /* お邪魔処理ラベル */
                    if (processGarbage(game)) {
                        game->is_gameover = 1;
                        fprintf(game->fp_out, "\a");
                        show_gameover_message(game);
                        wait_retry(game); 
                        return;
                    }
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
                    /* 接地していなければ1段下げる */
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
 * タスクエントリポイント
 * =========================================================================== */

void task1(void) {
    TetrisGame game1;
    game1.port_id = 0;
    game1.fp_out = com0out;
    game1.sync_generation = 0;
    all_games[0] = &game1; 
    wait_start(&game1);
    while(1) { run_tetris(&game1); }
}

void task2(void) {
    TetrisGame game2;
    game2.port_id = 1;
    game2.fp_out = com1out;
    game2.sync_generation = 0;
    all_games[1] = &game2;
    wait_start(&game2);
    while(1) { run_tetris(&game2); }
}

/* ===========================================================================
 * main関数
 * =========================================================================== */
int main(void) {
    init_kernel();
    com0in  = fdopen(0, "r");
    com0out = fdopen(1, "w");
    com1in  = fdopen(4, "r");
    com1out = fdopen(4, "w");
    
    set_task(task1);
    set_task(task2);
    
    begin_sch();
    return 0;
}