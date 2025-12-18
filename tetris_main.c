/* ===================================================================
 * tetris_main.c
 * テーマ3対応: イベント駆動型マルチタスク (Event-Driven)
 * * 機能: 
 * - イベントディスパッチャ (wait_event) の導入
 * - キー入力，タイマー，攻撃判定をイベントとして統一管理
 * - 落下速度調整 (20tick)
 * - お邪魔ブロックせり上がり位置修正
 * - 勝敗判定と画面制御
 * =================================================================== */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "mtk_c.h"

/* -------------------------------------------------------------------
 * ファイルポインタ参照
 * ------------------------------------------------------------------- */
extern FILE *com0in;
extern FILE *com0out;
extern FILE *com1in;
extern FILE *com1out;

/* -------------------------------------------------------------------
 * 外部関数
 * ------------------------------------------------------------------- */
extern void init_kernel(void);
extern void set_task(void (*func)());
extern void begin_sch(void);
extern int inbyte(int ch);
extern void skipmt(void);

/* -------------------------------------------------------------------
 * 定数・マクロ
 * ------------------------------------------------------------------- */
#define FIELD_WIDTH  12
#define FIELD_HEIGHT 22
#define MINO_WIDTH   4
#define MINO_HEIGHT  4

/* 落下スピード (20tick = 1.0秒) */
#define DROP_INTERVAL 1000

/* VT100 エスケープシーケンス */
#define ESC_CLS      "\x1b[2J"
#define ESC_HOME     "\x1b[H"
#define ESC_RESET    "\x1b[0m"
#define ESC_HIDE_CUR "\x1b[?25l"
#define ESC_SHOW_CUR "\x1b[?25h"
#define ESC_CLR_LINE "\x1b[K"

/* カラー定義 */
#define COL_CYAN     "\x1b[38;2;0;255;255m"
#define COL_YELLOW   "\x1b[38;2;255;255;0m"
#define COL_PURPLE   "\x1b[38;2;160;32;240m"
#define COL_BLUE     "\x1b[38;2;0;0;255m"
#define COL_ORANGE   "\x1b[38;2;255;165;0m"
#define COL_GREEN    "\x1b[38;2;0;255;0m"
#define COL_RED      "\x1b[38;2;255;0;0m"
#define COL_WHITE    "\x1b[38;2;255;255;255m"
#define COL_GRAY     "\x1b[38;2;128;128;128m"
#define BG_BLACK     "\x1b[40m"
#define COL_WALL     COL_WHITE

/* -------------------------------------------------------------------
 * イベント定義 (Theme 3: Event-Driven)
 * ------------------------------------------------------------------- */
typedef enum {
    EVT_NONE,
    EVT_KEY_INPUT,  /* キー入力があった */
    EVT_TIMER,      /* 自然落下の時間が来た */
    EVT_WIN,        /* 勝利条件を満たした */
    EVT_QUIT        /* 終了操作 */
} EventType;

typedef struct {
    EventType type;
    int param;      /* キーコードなどの付加情報 */
} Event;

/* -------------------------------------------------------------------
 * ゲーム状態構造体
 * ------------------------------------------------------------------- */
typedef struct {
    int port_id;
    FILE *fp_out;
    
    char field[FIELD_HEIGHT][FIELD_WIDTH];
    char displayBuffer[FIELD_HEIGHT][FIELD_WIDTH];
    char prevBuffer[FIELD_HEIGHT][FIELD_WIDTH];
    int force_refresh;

    int minoType;
    int minoAngle;
    int minoX;
    int minoY;
    
    int bag[7];
    int bag_index;

    unsigned long next_drop_time;
    
    int score;
    int lines_cleared;
    int pending_garbage;
    int is_gameover;

} TetrisGame;

TetrisGame *all_games[2] = {NULL, NULL};

/* -------------------------------------------------------------------
 * グローバルデータ
 * ------------------------------------------------------------------- */
enum { MINO_TYPE_I, MINO_TYPE_O, MINO_TYPE_S, MINO_TYPE_Z, MINO_TYPE_J, MINO_TYPE_L, MINO_TYPE_T, MINO_TYPE_GARBAGE, MINO_TYPE_MAX };
const char* minoColors[MINO_TYPE_MAX] = { COL_CYAN, COL_YELLOW, COL_GREEN, COL_RED, COL_BLUE, COL_ORANGE, COL_PURPLE, COL_GRAY };
enum { MINO_ANGLE_0, MINO_ANGLE_90, MINO_ANGLE_180, MINO_ANGLE_270, MINO_ANGLE_MAX };

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

/* -------------------------------------------------------------------
 * 基本ロジック関数
 * ------------------------------------------------------------------- */

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
    fprintf(game->fp_out, "SCORE: %-6d  LINES: %-4d  ATK: %d%s", 
            game->score, game->lines_cleared, game->pending_garbage, ESC_CLR_LINE);
    fprintf(game->fp_out, "\n--------------------------------%s", ESC_CLR_LINE);

    int offset_y = 3;
    for (i = 0; i < FIELD_HEIGHT; i++) {
        for (j = 0; j < FIELD_WIDTH; j++) {
            cellVal = game->displayBuffer[i][j];
            if (game->force_refresh || cellVal != game->prevBuffer[i][j]) {
                fprintf(game->fp_out, "\x1b[%d;%dH", i + offset_y, j * 2 + 1);
                if (cellVal == 0) fprintf(game->fp_out, "%s .%s", BG_BLACK, ESC_RESET);
                else if (cellVal == 1) fprintf(game->fp_out, "%s%s[]%s", BG_BLACK, COL_WALL, ESC_RESET);
                else if (cellVal >= 2) fprintf(game->fp_out, "%s%s[]%s", BG_BLACK, minoColors[cellVal - 2], ESC_RESET);
                
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

int processGarbage(TetrisGame *game) {
    int lines = game->pending_garbage;
    if (lines <= 0) return 0;
    if (lines > 4) lines = 4;
    game->pending_garbage -= lines;

    int i, j, k;
    /* 1. 上部チェック */
    for (k = 0; k < lines; k++) {
        for (j = 1; j < FIELD_WIDTH - 1; j++) {
            if (game->field[k][j] != 0) return 1; 
        }
    }
    /* 2. シフト (床残し) */
    for (i = 0; i < FIELD_HEIGHT - 1 - lines; i++) {
        memcpy(game->field[i], game->field[i + lines], FIELD_WIDTH);
    }
    /* 3. ゴミ生成 (床残し) */
    for (i = FIELD_HEIGHT - 1 - lines; i < FIELD_HEIGHT - 1; i++) {
        game->field[i][0] = 1;
        game->field[i][FIELD_WIDTH - 1] = 1;
        for (j = 1; j < FIELD_WIDTH - 1; j++) {
            game->field[i][j] = 2 + MINO_TYPE_GARBAGE; 
        }
        int hole = 1 + (tick + rand() + i) % (FIELD_WIDTH - 2);
        game->field[i][hole] = 0;
    }
    game->force_refresh = 1;
    return 0; 
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

/* -------------------------------------------------------------------
 * wait_event
 * イベント駆動の中核関数
 * イベントが発生するまで待ち続け(skipmt)，発生したらイベントを返す
 * ------------------------------------------------------------------- */
Event wait_event(TetrisGame *game) {
    Event e;
    e.type = EVT_NONE;
    int c;
    int opponent_id = (game->port_id == 0) ? 1 : 0;

    /* イベント待機ループ */
    while (1) {
        
        /* 1. 優先イベント: 勝利判定 (相手の死亡) */
        if (all_games[opponent_id] != NULL && all_games[opponent_id]->is_gameover) {
            e.type = EVT_WIN;
            return e;
        }

        /* 2. 入力イベント: ノンブロッキング入力チェック */
        c = inbyte(game->port_id);
        if (c != -1) {
            if (c == 'q') e.type = EVT_QUIT;
            else {
                e.type = EVT_KEY_INPUT;
                e.param = c;
            }
            return e;
        }

        /* 3. タイマーイベント: 自然落下時刻のチェック */
        if (tick >= game->next_drop_time) {
            /* 次回の時間を設定し，イベント発行 */
            game->next_drop_time = tick + DROP_INTERVAL; 
            e.type = EVT_TIMER;
            return e;
        }

        /* 4. イベントがない場合はCPUを譲渡して待機 (ビジーループ回避) */
        skipmt();
    }
}

/* -------------------------------------------------------------------
 * ゲームロジック (イベントループ化)
 * ------------------------------------------------------------------- */
void run_tetris(TetrisGame *game) {
    int i, j;
    
    /* 初期化 */
    game->score = 0;
    game->lines_cleared = 0;
    game->pending_garbage = 0;
    game->is_gameover = 0;
    game->force_refresh = 1;
    memset(game->prevBuffer, 0, sizeof(game->prevBuffer));
    game->bag_index = 7;

    fprintf(game->fp_out, ESC_CLS ESC_HIDE_CUR); 
    
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

    /* --- メインイベントループ --- */
    while (1) {
        /* イベントの取得 (発生するまで戻らない) */
        Event e = wait_event(game);

        /* イベントに応じた処理 (Switch Dispatch) */
        switch (e.type) {
            case EVT_WIN:
                show_victory_message(game);
                return;

            case EVT_QUIT:
                fprintf(game->fp_out, "%sQuit.\n", ESC_SHOW_CUR);
                return;

            case EVT_KEY_INPUT:
                /* キー操作処理 */
                switch (e.param) {
                    case 's':
                        if (!isHit(game, game->minoX, game->minoY + 1, game->minoType, game->minoAngle)) {
                            game->minoY++;
                            game->next_drop_time = tick + DROP_INTERVAL; /* タイマー延長 */
                        }
                        break;
                    case 'a':
                        if (!isHit(game, game->minoX - 1, game->minoY, game->minoType, game->minoAngle)) {
                            game->minoX--;
                        }
                        break;
                    case 'd':
                        if (!isHit(game, game->minoX + 1, game->minoY, game->minoType, game->minoAngle)) {
                            game->minoX++;
                        }
                        break;
                    case ' ':
                        if (!isHit(game, game->minoX, game->minoY, game->minoType, (game->minoAngle + 1) % MINO_ANGLE_MAX)) {
                            game->minoAngle = (game->minoAngle + 1) % MINO_ANGLE_MAX;
                        }
                        break;
                }
                display(game);
                break;

            case EVT_TIMER:
                /* 自然落下処理 */
                if (isHit(game, game->minoX, game->minoY + 1, game->minoType, game->minoAngle)) {
                    /* 固定・消去・攻撃・生成・判定など一連の更新処理 */
                    
                    /* 1. 固定 */
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
                    
                    /* 2. 消去 & 攻撃 */
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
                    
                    /* 3. お邪魔せり上がり */
                    if (processGarbage(game)) {
                        game->is_gameover = 1;
                        show_gameover_message(game);
                        return;
                    }

                    /* 4. 次のミノ生成 */
                    resetMino(game);
                    
                    /* 5. 窒息判定 */
                    if (isHit(game, game->minoX, game->minoY, game->minoType, game->minoAngle)) {
                        game->is_gameover = 1;
                        show_gameover_message(game);
                        return; 
                    }
                    
                } else {
                    /* 移動可能なら下へ */
                    game->minoY++;
                }
                display(game);
                break;
                
            default:
                break;
        }
    }
}

/* -------------------------------------------------------------------
 * タスク群
 * ------------------------------------------------------------------- */
void task1(void) {
    TetrisGame game1;
    game1.port_id = 0;
    game1.fp_out = com0out;
    all_games[0] = &game1; 
    
    while(1) {
        run_tetris(&game1);
        int i;
        for(i=0; i<200000; i++) if (i % 100 == 0) skipmt(); 
    }
}

void task2(void) {
    TetrisGame game2;
    game2.port_id = 1;
    game2.fp_out = com1out;
    all_games[1] = &game2;
    
    while(1) {
        run_tetris(&game2);
        int i;
        for(i=0; i<200000; i++) if (i % 100 == 0) skipmt(); 
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