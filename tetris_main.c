/* ===================================================================
 * tetris_main.c (Diff Render Ver.)
 * 差分描画適用・入力ロジックは維持
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

/* 落下スピード */
#define DROP_INTERVAL 5

/* VT100 エスケープシーケンス */
#define ESC_CLS      "\x1b[2J"      /* 画面クリア */
#define ESC_HOME     "\x1b[H"       /* カーソルを左上へ */
#define ESC_RESET    "\x1b[0m"      /* 色リセット */
#define ESC_HIDE_CUR "\x1b[?25l"    /* カーソル非表示 */
#define ESC_SHOW_CUR "\x1b[?25h"    /* カーソル表示 */
#define ESC_CLR_LINE "\x1b[K"       /* 行末までクリア */

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
    
    /* 差分描画用バッファとフラグ */
    char prevBuffer[FIELD_HEIGHT][FIELD_WIDTH];
    int force_refresh;

    /* ミノの状態 */
    int minoType;
    int minoAngle;
    int minoX;
    int minoY;
    
    /* ゲーム進行管理 */
    unsigned long next_drop_time;
    unsigned int random_seed; 
    
    /* スコア管理 */
    int score;
    int lines_cleared;
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

/* 画面描画 (差分更新による高速化版) */
void display(TetrisGame *game) {
    int i, j;
    char cellVal;
    int changes = 0;

    /* 1. 現在の盤面状態を作成 (displayBufferへ) */
    memcpy(game->displayBuffer, game->field, sizeof(game->field));

    /* ミノをバッファに書き込み */
    for (i = 0; i < MINO_HEIGHT; i++) {
        for (j = 0; j < MINO_WIDTH; j++) {
            if (minoShapes[game->minoType][game->minoAngle][i][j]) {
                if (game->minoY + i >= 0 && game->minoY + i < FIELD_HEIGHT &&
                    game->minoX + j >= 0 && game->minoX + j < FIELD_WIDTH) {
                    game->displayBuffer[game->minoY + i][game->minoX + j] = 2 + game->minoType;
                }
            }
        }
    }

    /* 2. スコア表示 (ここは毎回更新) */
    /* カーソルを左上(1,1)へ移動 */
    fprintf(game->fp_out, "\x1b[1;1H"); 
    fprintf(game->fp_out, "SCORE: %-6d  LINES: %-4d%s", 
            game->score, game->lines_cleared, ESC_CLR_LINE);
    fprintf(game->fp_out, "\n--------------------------");

    /* 3. 盤面の差分描画 */
    /* 盤面は3行目から始まると仮定 (スコア表示:1行目, 区切り:2行目) */
    int offset_y = 3; 

    for (i = 0; i < FIELD_HEIGHT; i++) {
        for (j = 0; j < FIELD_WIDTH; j++) {
            cellVal = game->displayBuffer[i][j];

            /* 強制再描画フラグが立っているか、内容が変化していたら描画する */
            if (game->force_refresh || cellVal != game->prevBuffer[i][j]) {
                
                /* カーソルをピンポイントで移動 ( VT100: \x1b[行;列H ) */
                /* 行: i + offset_y, 列: j * 2 + 1 (半角2文字分なのでx2, 1始まり) */
                fprintf(game->fp_out, "\x1b[%d;%dH", i + offset_y, j * 2 + 1);

                /* マスの描画 */
                if (cellVal == 0) {
                    fprintf(game->fp_out, " .");
                } else if (cellVal == 1) {
                    fprintf(game->fp_out, "%s[]%s", COL_WALL, ESC_RESET);
                } else if (cellVal >= 2 && cellVal <= 8) {
                    fprintf(game->fp_out, "%s[]%s", minoColors[cellVal - 2], ESC_RESET);
                } else {
                    fprintf(game->fp_out, "??");
                }

                /* 描画した内容を記憶 */
                game->prevBuffer[i][j] = cellVal;
                changes++;
            }
        }
    }
    
    game->force_refresh = 0; /* 強制フラグを下ろす */
    
    /* 変更があった場合のみフラッシュ */
    if (changes > 0) {
        fflush(game->fp_out);
    }
}

/* 当たり判定 */
int isHit(TetrisGame *game, int _minoX, int _minoY, int _minoType, int _minoAngle) {
    int i, j;
    for (i = 0; i < MINO_HEIGHT; i++) {
        for (j = 0; j < MINO_WIDTH; j++) {
            if (minoShapes[_minoType][_minoAngle][i][j]) {
                int fy = _minoY + i;
                int fx = _minoX + j;
                
                /* 範囲外チェック */
                if (fy < 0 || fy >= FIELD_HEIGHT || fx < 0 || fx >= FIELD_WIDTH) {
                    return 1;
                }
                /* 既にブロックがあるか */
                if (game->field[fy][fx]) {
                    return 1;
                }
            }
        }
    }
    return 0;
}

/* ミノのリセット */
void resetMino(TetrisGame *game) {
    game->minoX = 5;
    game->minoY = 0;
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
    game->score = 0;
    game->lines_cleared = 0;
    
    /* ★追加: 差分描画用の初期化 */
    game->force_refresh = 1;
    memset(game->prevBuffer, 0, sizeof(game->prevBuffer));

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

    game->next_drop_time = tick + DROP_INTERVAL;

    while (1) {
        /* 1. キー入力処理 (元のロジック維持: 1ループ1入力処理) */
        c = inbyte(game->port_id);

        if (c != -1) {
            switch (c) {
                case 's': /* 下 */
                    if (!isHit(game, game->minoX, game->minoY + 1, game->minoType, game->minoAngle)) {
                        game->minoY++;
                        game->next_drop_time  = tick + DROP_INTERVAL;
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
                case 'q': /* 強制終了 */
                    fprintf(game->fp_out, "%sQuit.\n", ESC_SHOW_CUR);
                    return;
            }
            display(game);
        }

        /* 2. 時間経過による落下処理 */
        if (tick >= game->next_drop_time) {
            game->next_drop_time = tick + DROP_INTERVAL;

            if (isHit(game, game->minoX, game->minoY + 1, game->minoType, game->minoAngle)) {
                /* 固定処理 */
                for (i = 0; i < MINO_HEIGHT; i++) {
                    for (j = 0; j < MINO_WIDTH; j++) {
                        if (minoShapes[game->minoType][game->minoAngle][i][j]) {
                            if (game->minoY + i >= 0 && game->minoY + i < FIELD_HEIGHT &&
                                game->minoX + j >= 0 && game->minoX + j < FIELD_WIDTH) {
                                game->field[game->minoY + i][game->minoX + j] = 2 + game->minoType;
                            }
                        }
                    }
                }
                
                /* ライン消去判定 & スコア計算 */
                {
                    int lines_this_turn = 0;
                    
                    for (i = 0; i < FIELD_HEIGHT - 1; i++) {
                        int lineFill = 1;
                        for (j = 1; j < FIELD_WIDTH - 1; j++) {
                            if (game->field[i][j] == 0) {
                                lineFill = 0; break;
                            }
                        }
                        if (lineFill) {
                            int k;
                            /* ラインを消して上から詰める */
                            for (k = i; k > 0; k--) {
                                memcpy(game->field[k], game->field[k - 1], FIELD_WIDTH);
                            }
                            memset(game->field[0], 0, FIELD_WIDTH);
                            game->field[0][0] = game->field[0][FIELD_WIDTH-1] = 1;
                            
                            lines_this_turn++;
                        }
                    }

                    /* スコア加算処理 */
                    if (lines_this_turn > 0) {
                        game->lines_cleared += lines_this_turn;
                        
                        /* ライン消去時は画面が大きく変わるので強制再描画 */
                        game->force_refresh = 1;

                        switch (lines_this_turn) {
                            case 1: game->score += 100; break;
                            case 2: game->score += 300; break;
                            case 3: game->score += 500; break;
                            case 4: game->score += 800; break;
                            default: game->score += 100 * lines_this_turn; break;
                        }
                    }
                }
                
                resetMino(game);
                
                /* ゲームオーバー判定 */
                if (isHit(game, game->minoX, game->minoY, game->minoType, game->minoAngle)) {
                    fprintf(game->fp_out, "\n%sGAME OVER\n", COL_RED);
                    fprintf(game->fp_out, "Final Score: %d%s\n", game->score, ESC_RESET);
                    return; 
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
    game1.fp_out = com0out;
    
    /* 無限ループでゲームを回す */
    while(1) {
        run_tetris(&game1);
        /* ゲームオーバー後，少し待って再スタート */
        int i;
        for(i=0; i<100000; i++){
            if (i % 100 == 0) {
                skipmt();
            }
        }
    }
}

/* タスク2 (Port 1) */
void task2(void) {
    TetrisGame game2;
    
    /* Game2 の設定 */
    game2.port_id = 1;
    game2.fp_out = com1out;
    
    while(1) {
        run_tetris(&game2);
        int i;
        for(i=0; i<100000; i++){
            if (i % 100 == 0) {
                skipmt();
            }
        }
    }
}

/* -------------------------------------------------------------------
 * main
 * ------------------------------------------------------------------- */
int main(void) {
    init_kernel();

    /* ストリームの割り当て */
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