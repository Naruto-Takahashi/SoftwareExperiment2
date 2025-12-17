/* ===================================================================
 * tetris_main.c (Battle Ver.)
 * 差分描画 + 7種1巡 + 色変更 + 対戦(お邪魔ブロック)
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
#define COL_YELLOW   "\x1b[33m"     /* L: 黄色 */
#define COL_GREEN    "\x1b[32m"     /* S: 緑 */
#define COL_RED      "\x1b[31m"     /* Z: 赤 */
#define COL_BLUE     "\x1b[34m"     /* J: 青 */
#define COL_MAGENTA  "\x1b[35m"     /* T: 紫 */
#define COL_WHITE    "\x1b[37m"     /* O: 白 */
#define COL_GRAY     "\x1b[37m"     /* お邪魔: 白 */

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
    
    /* 7種1巡(7-Bag)システム用 */
    int bag[7];
    int bag_index;

    /* ゲーム進行管理 */
    unsigned long next_drop_time;
    unsigned int random_seed; 
    
    /* スコア管理 */
    int score;
    int lines_cleared;

    /* 対戦用パラメータ */
    int pending_garbage;    /* 相手から送られてきた、まだ処理していないお邪魔段数 */
    int is_gameover;        /* ゲームオーバーフラグ */

} TetrisGame;

/* タスク間でお互いを参照するためのグローバルポインタ配列 */
TetrisGame *all_games[2] = {NULL, NULL};

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
    MINO_TYPE_GARBAGE,
    MINO_TYPE_MAX
};

/* 色コードの配列 */
const char* minoColors[MINO_TYPE_MAX] = {
    COL_CYAN,    /* I */
    COL_WHITE,   /* O */
    COL_GREEN,   /* S */
    COL_RED,     /* Z */
    COL_BLUE,    /* J */
    COL_YELLOW,  /* L */
    COL_MAGENTA, /* T */
    COL_GRAY     /* Garbage */
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
    /* MINO_TYPE_GARBAGE (ダミー定義) */
    {
        {{0}}, {{0}}, {{0}}, {{0}}
    }
};

/* -------------------------------------------------------------------
 * 関数群
 * ------------------------------------------------------------------- */

/* 画面描画 */
void display(TetrisGame *game) {
    int i, j;
    char cellVal;
    int changes = 0;

    memcpy(game->displayBuffer, game->field, sizeof(game->field));

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

    fprintf(game->fp_out, "\x1b[1;1H"); 
    /* お邪魔のpending数も表示してあげると親切 */
    fprintf(game->fp_out, "SCORE: %-6d  LINES: %-4d  ATK: %d%s", 
            game->score, game->lines_cleared, game->pending_garbage, ESC_CLR_LINE);
    fprintf(game->fp_out, "\n--------------------------------%s", ESC_CLR_LINE);

    int offset_y = 3; 

    for (i = 0; i < FIELD_HEIGHT; i++) {
        for (j = 0; j < FIELD_WIDTH; j++) {
            cellVal = game->displayBuffer[i][j];

            if (game->force_refresh || cellVal != game->prevBuffer[i][j]) {
                fprintf(game->fp_out, "\x1b[%d;%dH", i + offset_y, j * 2 + 1);

                if (cellVal == 0) {
                    fprintf(game->fp_out, " .");
                } else if (cellVal == 1) {
                    fprintf(game->fp_out, "%s[]%s", COL_WALL, ESC_RESET);
                } else if (cellVal >= 2 && cellVal <= 9) { /* 9=GARBAGE */
                    fprintf(game->fp_out, "%s[]%s", minoColors[cellVal - 2], ESC_RESET);
                } else {
                    fprintf(game->fp_out, "??");
                }

                game->prevBuffer[i][j] = cellVal;
                changes++;
            }
        }
    }
    
    game->force_refresh = 0;
    if (changes > 0) fflush(game->fp_out);
}

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

void fillBag(TetrisGame *game) {
    int i, j, temp;
    for (i = 0; i < 7; i++) game->bag[i] = i;
    for (i = 6; i > 0; i--) {
        j = (tick + rand()) % (i + 1); 
        temp = game->bag[i]; game->bag[i] = game->bag[j]; game->bag[j] = temp;
    }
    game->bag_index = 0;
}

void resetMino(TetrisGame *game) {
    game->minoX = 5;
    game->minoY = 0;
    if (game->bag_index >= 7) fillBag(game);
    game->minoType = game->bag[game->bag_index];
    game->bag_index++;
    game->minoAngle = (tick + rand()) % MINO_ANGLE_MAX;
}

/* お邪魔ブロックのせり上がり処理 */
int processGarbage(TetrisGame *game) {
    int lines = game->pending_garbage;
    if (lines <= 0) return 0;

    /* 溜まっている分だけループ (一度に全部処理するか、少しずつかは調整可) */
    /* 今回はゲーム性を考慮して、一度に最大4段までとする */
    if (lines > 4) lines = 4;

    game->pending_garbage -= lines;

    int i, j, k;
    
    /* 1. 上部にはみ出すブロックがないかチェック (あるならゲームオーバー) */
    for (k = 0; k < lines; k++) {
        for (j = 1; j < FIELD_WIDTH - 1; j++) {
            if (game->field[k][j] != 0) return 1; /* 死 */
        }
    }

    /* 2. フィールド全体を上にずらす */
    for (i = 0; i < FIELD_HEIGHT - lines; i++) {
        memcpy(game->field[i], game->field[i + lines], FIELD_WIDTH);
    }

    /* 3. 下からお邪魔ブロックを生成 */
    for (i = FIELD_HEIGHT - lines; i < FIELD_HEIGHT; i++) {
        /* 壁 */
        game->field[i][0] = 1;
        game->field[i][FIELD_WIDTH - 1] = 1;
        
        /* お邪魔ブロックで埋める */
        for (j = 1; j < FIELD_WIDTH - 1; j++) {
            game->field[i][j] = 2 + MINO_TYPE_GARBAGE; 
        }
        
        /* ランダムな穴を1つ空ける */
        int hole = 1 + (tick + rand() + i) % (FIELD_WIDTH - 2);
        game->field[i][hole] = 0;
    }

    game->force_refresh = 1; /* 画面全体更新 */
    return 0; /* 生存 */
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
    game->pending_garbage = 0; /* お邪魔リセット */
    game->is_gameover = 0;
    
    game->force_refresh = 1;
    memset(game->prevBuffer, 0, sizeof(game->prevBuffer));
    
    game->bag_index = 7;

    fprintf(game->fp_out, ESC_CLS);      
    fprintf(game->fp_out, ESC_HIDE_CUR); 
    
    memset(game->field, 0, sizeof(game->field));
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
        /* 1. キー入力処理 */
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
                case 'q':
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
                
                /* ライン消去判定 & 攻撃処理 */
                {
                    int lines_this_turn = 0;
                    for (i = 0; i < FIELD_HEIGHT - 1; i++) {
                        int lineFill = 1;
                        for (j = 1; j < FIELD_WIDTH - 1; j++) {
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
                        game->lines_cleared += lines_this_turn;
                        game->force_refresh = 1;
                        
                        /* ★追加: 攻撃処理 (相手にお邪魔を送る) */
                        int attack = 0;
                        if (lines_this_turn == 2) attack = 1;
                        if (lines_this_turn == 3) attack = 2;
                        if (lines_this_turn == 4) attack = 4;
                        
                        if (attack > 0) {
                            int opponent_id = (game->port_id == 0) ? 1 : 0;
                            if (all_games[opponent_id] != NULL && !all_games[opponent_id]->is_gameover) {
                                all_games[opponent_id]->pending_garbage += attack;
                            }
                        }

                        switch (lines_this_turn) {
                            case 1: game->score += 100; break;
                            case 2: game->score += 300; break;
                            case 3: game->score += 500; break;
                            case 4: game->score += 800; break;
                        }
                    }
                }
                
                /* ★追加: お邪魔ブロックのせり上がり処理 (ブロック固定後に行う) */
                if (processGarbage(game)) {
                    /* せり上がりで死んだ場合 */
                    fprintf(game->fp_out, "\n%sGAME OVER (Garbage)\n", COL_RED);
                    game->is_gameover = 1;
                    return;
                }

                resetMino(game);
                
                /* ゲームオーバー判定 */
                if (isHit(game, game->minoX, game->minoY, game->minoType, game->minoAngle)) {
                    fprintf(game->fp_out, "\n%sGAME OVER\n", COL_RED);
                    fprintf(game->fp_out, "Final Score: %d%s\n", game->score, ESC_RESET);
                    game->is_gameover = 1;
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
void task1(void) {
    TetrisGame game1;
    game1.port_id = 0;
    game1.fp_out = com0out;
    all_games[0] = &game1; /* 自分のポインタを登録 */
    
    while(1) {
        run_tetris(&game1);
        int i;
        for(i=0; i<100000; i++){ if (i % 100 == 0) skipmt(); }
    }
}

void task2(void) {
    TetrisGame game2;
    game2.port_id = 1;
    game2.fp_out = com1out;
    all_games[1] = &game2; /* 自分のポインタを登録 */
    
    while(1) {
        run_tetris(&game2);
        int i;
        for(i=0; i<100000; i++){ if (i % 100 == 0) skipmt(); }
    }
}

/* -------------------------------------------------------------------
 * main
 * ------------------------------------------------------------------- */
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