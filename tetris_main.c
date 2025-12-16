/* ===================================================================
 * tetris_main.c
 * M68k実験ボード用テトリスゲーム本体
 * =================================================================== */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "mtk_c.h"

/* -------------------------------------------------------------------
 * ファイルポインタの参照 (extern)
 * ------------------------------------------------------------------- */
extern FILE *com0in;
extern FILE *com0out;
extern FILE *com1in;
extern FILE *com1out;

/* -------------------------------------------------------------------
 * 外部関数の宣言
 * ------------------------------------------------------------------- */
extern void init_kernel(void);
extern void set_task(void (*func)());
extern void begin_sch(void);
extern int inbyte(int ch);      /* ノンブロッキング入力 */
extern void skipmt(void);       /* CPU譲渡 */

/* -------------------------------------------------------------------
 * 定数・マクロ定義
 * ------------------------------------------------------------------- */
#define FIELD_WIDTH  12
#define FIELD_HEIGHT 22

#define MINO_WIDTH   4
#define MINO_HEIGHT  4

/* VT100 エスケープシーケンス */
#define ESC_CLS      "\x1b[2J"      /* 画面クリア */
#define ESC_HOME     "\x1b[H"       /* カーソルを左上へ */
#define ESC_RESET    "\x1b[0m"      /* 色リセット */
#define ESC_HIDE_CUR "\x1b[?25l"    /* カーソル非表示 */
#define ESC_SHOW_CUR "\x1b[?25h"    /* カーソル表示 */

/* 色定義 (文字色) */
#define COL_CYAN     "\x1b[36m"     /* I: 水色 */
#define COL_YELLOW   "\x1b[33m"     /* O: 黄色 */
#define COL_GREEN    "\x1b[32m"     /* S: 緑 */
#define COL_RED      "\x1b[31m"     /* Z: 赤 */
#define COL_BLUE     "\x1b[34m"     /* J: 青 */
#define COL_MAGENTA  "\x1b[35m"     /* L: 紫 */
#define COL_WHITE    "\x1b[37m"     /* T: 白 */
#define COL_WALL     "\x1b[37m"     /* 壁: 白 */

/* -------------------------------------------------------------------
 * ゲーム状態を管理する構造体の定義
 * ------------------------------------------------------------------- */
typedef struct {
    int port_id;            /* 0 or 1 */
    FILE *fp_out;           /* 出力用ファイルポインタ */
    
    char field[FIELD_HEIGHT][FIELD_WIDTH];
    char displayBuffer[FIELD_HEIGHT][FIELD_WIDTH];
    
    int minoType;
    int minoAngle;
    int minoX;
    int minoY;
    
    unsigned long next_drop_time;
    unsigned int random_seed; /* 乱数シードも個別に持つ */
} TetrisGame;

/* -------------------------------------------------------------------
 * グローバル変数
 * ------------------------------------------------------------------- */

/* ミノの定義 */
enum {
    MINO_TYPE_I,
    MINO_TYPE_O,
    MINO_TYPE_S,
    MINO_TYPE_Z,
    MINO_TYPE_J,
    MINO_TYPE_L,
    MINO_TYPE_T,
    MINO_TYPE_MAX
};

/* 色コードの配列 (MINO_TYPEの順に対応) */
const char* minoColors[MINO_TYPE_MAX] = {
    COL_CYAN,    /* I */
    COL_YELLOW,  /* O */
    COL_GREEN,   /* S */
    COL_RED,     /* Z */
    COL_BLUE,    /* J */
    COL_MAGENTA, /* L */
    COL_WHITE    /* T */
};

enum {
    MINO_ANGLE_0,
    MINO_ANGLE_90,
    MINO_ANGLE_180,
    MINO_ANGLE_270,
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
    }
};

/* -------------------------------------------------------------------
 * 関数群
 * ------------------------------------------------------------------- */

/* 画面描画 */
void display(TetrisGame *game) {
    int i, j;
    char cellVal;

    /* フィールドをバッファにコピー */
    memcpy(game->displayBuffer, game->field, sizeof(game->field));

    /* ミノをバッファに書き込み */
    for (i = 0; i < MINO_HEIGHT; i++) {
        for (j = 0; j < MINO_WIDTH; j++) {
            if (minoShapes[game->minoType][game->minoAngle][i][j]) {
                game->displayBuffer[game->minoY + i][game->minoX + j] = 2 + game->minoType;
            }
        }
    }

    /* カーソルを左上に戻す (fprintfを使用) */
    fprintf(game->fp_out, ESC_HOME);

    /* 描画ループ */
    for (i = 0; i < FIELD_HEIGHT; i++) {
        for (j = 0; j < FIELD_WIDTH; j++) {
            cellVal = game->displayBuffer[i][j];
            
            /* 出力はすべて game->fp_out に対して行う */
            if (cellVal == 0) {
                fprintf(game->fp_out, " .");
            } else if (cellVal == 1) {
                fprintf(game->fp_out, "%s[]%s", COL_WALL, ESC_RESET);
            } else if (cellVal >= 2 && cellVal <= 8) {
                fprintf(game->fp_out, "%s[]%s", minoColors[cellVal - 2], ESC_RESET);
            } else {
                fprintf(game->fp_out, "??");
            }
        }
        fprintf(game->fp_out, "\n");
    }
    
    /* バッファをフラッシュして即時表示させる */
    fflush(game->fp_out);
}

/* 当たり判定 (構造体対応版) */
int isHit(TetrisGame *game, int _minoX, int _minoY, int _minoType, int _minoAngle) {
    int i, j;
    for (i = 0; i < MINO_HEIGHT; i++) {
        for (j = 0; j < MINO_WIDTH; j++) {
            if (minoShapes[_minoType][_minoAngle][i][j] &&
                game->field[_minoY + i][_minoX + j]) {
                return 1; 
            }
        }
    }
    return 0;
}

/* ミノのリセット (構造体対応版) */
void resetMino(TetrisGame *game) {
    game->minoX = 5;
    game->minoY = 0;
    /* 個別のシードを使って乱数生成 (簡易的な線形合同法などを使うか，rand() + tick) */
    game->minoType = (tick + rand()) % MINO_TYPE_MAX; 
    game->minoAngle = (tick + rand()) % MINO_ANGLE_MAX;
}

/* -------------------------------------------------------------------
 * 汎用ゲームロジック
 * ------------------------------------------------------------------- */
void run_tetris(TetrisGame *game) {
    int c;
    int i, j;

    /* 初期化 */
    fprintf(game->fp_out, ESC_CLS);      
    fprintf(game->fp_out, ESC_HIDE_CUR); 
    
    memset(game->field, 0, sizeof(game->field));
    
    /* 壁と床の作成 */
    for (i = 0; i < FIELD_HEIGHT; i++) {
        game->field[i][0] = game->field[i][FIELD_WIDTH - 1] = 1;
    }
    for (i = 0; i < FIELD_WIDTH; i++) {
        game->field[FIELD_HEIGHT - 1][i] = 1;
    }

    resetMino(game);
    display(game);

    game->next_drop_time = tick + 1;

    while (1) {
        /* 1. キー入力処理 (ポートIDを指定して入力) */
        c = inbyte(game->port_id);

        if (c != -1) {
            switch (c) {
                case 's': /* 下 */
                    if (!isHit(game, game->minoX, game->minoY + 1, game->minoType, game->minoAngle)) {
                        game->minoY++;
                    }
                    break;
                case 'a': /* 左 */
                    if (!isHit(game, game->minoX - 1, game->minoY, game->minoType, game->minoAngle)) {
                        game->minoX--;
                    }
                    break;
                case 'd': /* 右 */
                    if (!isHit(game, game->minoX + 1, game->minoY, game->minoType, game->minoAngle)) {
                        game->minoX++;
                    }
                    break;
                case ' ': /* 回転 */
                    if (!isHit(game, game->minoX, game->minoY, game->minoType, (game->minoAngle + 1) % MINO_ANGLE_MAX)) {
                        game->minoAngle = (game->minoAngle + 1) % MINO_ANGLE_MAX;
                    }
                    break;
                /* 追加: 強制終了やリセット機能など */
            }
            display(game);
        }

        /* 2. 時間経過による落下処理 */
        if (tick >= game->next_drop_time) {
            game->next_drop_time = tick + 5; /* 速度調整 */

            if (isHit(game, game->minoX, game->minoY + 1, game->minoType, game->minoAngle)) {
                /* 固定処理 */
                for (i = 0; i < MINO_HEIGHT; i++) {
                    for (j = 0; j < MINO_WIDTH; j++) {
                        if (minoShapes[game->minoType][game->minoAngle][i][j]) {
                            game->field[game->minoY + i][game->minoX + j] = 2 + game->minoType;
                        }
                    }
                }
                
                /* ライン消去判定 (game->field を操作) */
                for (i = 0; i < FIELD_HEIGHT - 1; i++) {
                    int lineFill = 1;
                    for (j = 1; j < FIELD_WIDTH - 1; j++) {
                        if (game->field[i][j] == 0) {
                            lineFill = 0; break;
                        }
                    }
                    if (lineFill) {
                        int k;
                        for (k = i; k > 0; k--) {
                            memcpy(game->field[k], game->field[k - 1], FIELD_WIDTH);
                        }
                        memset(game->field[0], 0, FIELD_WIDTH);
                        game->field[0][0] = game->field[0][FIELD_WIDTH-1] = 1;
                    }
                }
                
                resetMino(game);
                
                /* ゲームオーバー判定 */
                if (isHit(game, game->minoX, game->minoY, game->minoType, game->minoAngle)) {
                    fprintf(game->fp_out, "GAME OVER\n");
                    /* ここでリセット処理など */
                    return; /* または break して再初期化 */
                }
                
            } else {
                game->minoY++;
            }
            display(game);
        }

        /* 3. CPU譲渡 */
        skipmt();
    }
}

/* -------------------------------------------------------------------
 * タスク群
 * ------------------------------------------------------------------- */
/* タスク1 (Port 0) */
void task1(void) {
    TetrisGame game1;
    
    /* Game1 の設定 */
    game1.port_id = 0;
    game1.fp_out = com0out; /* mainでfdopenしたストリーム */
    
    /* 無限ループでゲームを回す */
    while(1) {
        run_tetris(&game1);
        /* ゲームオーバー後，再スタートまでのウェイトなどを入れると良い */
    }
}

/* タスク2 (Port 1) */
void task2(void) {
    TetrisGame game2;
    
    /* Game2 の設定 */
    game2.port_id = 1;
    game2.fp_out = com1out; /* mainでfdopenしたストリーム */
    
    while(1) {
        run_tetris(&game2);
    }
}

/* -------------------------------------------------------------------
 * main
 * ------------------------------------------------------------------- */
int main(void) {
    init_kernel();

    /* ストリームの割り当て (test3.c を参考) */
    com0in  = fdopen(0, "r");
    com0out = fdopen(1, "w");
    com1in  = fdopen(4, "r");
    com1out = fdopen(4, "w");

    /* タスク登録 */
    set_task(task1);
    set_task(task2);

    begin_sch();
    return 0;
}
