/******************************************************/
/* 各種レジスタ定義
/******************************************************/
*******************
** レジスタ群の先頭
*******************
.equ REGBASE, 0xFFF000 | DMAP を使用．
.equ IOBASE,  0x00d00000

*************************
** 割り込み関係のレジスタ
*************************
.equ IVR, REGBASE+0x300 | 割り込みベクタレジスタ
.equ IMR, REGBASE+0x304 | 割り込みマスクレジスタ
.equ ISR, REGBASE+0x30c | 割り込みステータスレジスタ
.equ IPR, REGBASE+0x310 | 割り込みペンディングレジスタ

***********************
** タイマ関係のレジスタ
***********************
.equ TCTL1,  REGBASE+0x600 | タイマ１コントロールレジスタ
.equ TPRER1, REGBASE+0x602 | タイマ１プリスケーラレジスタ
.equ TCMP1,  REGBASE+0x604 | タイマ１コンペアレジスタ
.equ TCN1,   REGBASE+0x608 | タイマ１カウンタレジスタ
.equ TSTAT1, REGBASE+0x60a | タイマ１ステータスレジスタ

********************************
** UART1（送受信）関係のレジスタ
********************************
.equ USTCNT1, REGBASE+0x900 | UART1 ステータス/コントロールレジスタ
.equ UBAUD1,  REGBASE+0x902 | UART1 ボーコントロールレジスタ
.equ URX1,    REGBASE+0x904 | UART1 受信レジスタ
.equ UTX1,    REGBASE+0x906 | UART1 送信レジスタ

***********
** LED
***********
.equ LED7, IOBASE+0x000002f | ボード搭載の LED 用レジスタ
.equ LED6, IOBASE+0x000002d | 使用法については付録 A.4.3.1
.equ LED5, IOBASE+0x000002b
.equ LED4, IOBASE+0x0000029
.equ LED3, IOBASE+0x000003f
.equ LED2, IOBASE+0x000003d
.equ LED1, IOBASE+0x000003b
.equ LED0, IOBASE+0x0000039

/******************************************************/
/*  スタック領域の確保
/******************************************************/
.section .bss
	.even
SYS_STK:
	.ds.b 0x4000 | システムスタック領域
	.even
SYS_STK_TOP:         | システムスタック領域の最後尾

/******************************************************/
/* 初期化
/******************************************************/
.section .text
	.even
.extern start
.global monitor_begin
monitor_begin: 
	move.w #0x2700,%SR      | 各種設定中の割り込み禁止
	lea.l SYS_STK_TOP, %SP  | Set SSP

*******************************
** 割り込みコントローラの初期化
*******************************
	move.b #0x40, IVR      | ユーザ割り込みベクタ番号を
	                       | 0x40+level に設定．
	move.l #0x00ff3ff9,IMR | 全割り込みマスク

******************************
** 送受信 (UART1) 関係の初期化 (割り込みレベルは 4 に固定されている) 
******************************
	move.w #0x0000, USTCNT1 | リセット
	move.w #0xe108, USTCNT1 | 送受信可能, パリティなし, 1 stop, 8 bit,
	                        | 送受割り込み禁止
	move.w #0x0038, UBAUD1  | baud rate = 230400 bps

*********************
** タイマ関係の初期化 (割り込みレベルは 6 に固定されている)
*********************
	move.w #0x0004, TCTL1  | restart, 割り込み不可,
	                       | システムクロックの 1/16 を単位として計時，
                               | タイマ使用停止

**************************************
** インターフェースの割り込みベクタ設定
**************************************
	move.l #UART_INTERFACE,        0x110 | レベル4ユーザ割り込みに設定
	move.l #TIMER_INTERFACE,       0x118 | レベル6ユーザ割り込みに設定
	move.l #SYSTEM_CALL_INTERFACE, 0x080 | TRAP0に設定

*********************
** キューの初期化
*********************
	jsr Init_Q
	** 走行レベルの設定
	move.w #0x0000, %SR   | LEVEL 0
	lea.l USR_STK_TOP,%SP | user stack の設定
	** システムコールによる RESET_TIMER の起動
	move.l #SYSCALL_NUM_RESET_TIMER,%D0
	trap #0
	jmp start
	
/******************************************************/
/* 送受信用キュー
/******************************************************/
****************
** 定数定義
****************
.section .data
	.even
/* キューの各要素のオフセットを EQU でシンボル化 */
	.equ     top,    0
	.equ     bottom, 255
	.equ     in,     256
	.equ     out,    260
	.equ     s,      264
	.equ     Q_SIZE, 266
/* キューの各先頭アドレスを EQU でシンボル化 */
.section .bss
	.even 
Q_BASE: .ds.b 1000

****************
** Init_Q
****************
.section .text
	.even
Init_Q:
	movem.l   %d0-%d2/%a4, -(%sp)
	moveq     #0, %d0
	move.w    #Q_SIZE, %d1
Init_Q_Loop:
	/* D2 = キュー #D0 のオフセット計算 (D0 * Q_SIZE) */
	move.w    %d0, %d2
	mulu.w    %d1, %d2
	/* A4 = キュー #D0 の先頭アドレス (topx) */
	movea.l   #Q_BASE, %a4
	adda.l    %d2, %a4
	/* inとoutの初期化 */
	move.l    %a4, in(%a4)
	move.l    %a4, out(%a4)
	/* sの初期化 */
	move.w    #0x0000, s(%a4)
	addq.w    #1, %d0
	cmpi.w    #2, %d0
	blt       Init_Q_Loop
	movem.l   (%sp)+, %d0-%d2/%a4
	rts

****************
** INQ
****************
INQ:
	move.w  %SR, -(%sp)
	move.w  #0x2700, %SR
	movem.l   %d2-%d5/%a1-%a3, -(%sp)
PUT_BUF:
	/* D3にQ_SIZEを準備 (MULU用) */
	move.w  #Q_SIZE, %d3
	/* D2 = キュー #D0 のオフセット計算 (D0 * Q_SIZE) */
	move.l  %d0, %d2           | D2 = キュー番号
	mulu.w  %d3, %d2           | D2 = D2 * Q_SIZE
	/* A1 = キュー #D5 の先頭アドレス (topx) */
	movea.l #Q_BASE, %a1
	adda.l  %d2,%a1            | A1 = Q_BASE + オフセット (ベースアドレス)
	/* sのロードと満杯チェック */
	move.w    s(%a1), %d4
	cmpi.w    #256,%d4         | s=256なら満杯（書き込み不可）
	beq       PUT_BUF_Fail
	/* データ書き込み */
	movea.l   in(%a1), %a2
	move.b    %d1,(%a2)+       | D1のデータを(A2)に
	/* ラップアラウンドチェック */
	lea.l     bottom(%a1), %a3
	cmpa.l    %a3, %a2
	bls       PUT_BUF_STEP     | A2 <= A3 ならOK
	/* ラップアラウンド処理: A2 = topx (A1) */
	movea.l   %a1, %a2
PUT_BUF_STEP:
	/* in更新 */
	move.l  %a2, in(%a1)
	/* s のインクリメントと更新 */
	addq.w  #1, %d4
	move.w  %d4, s(%a1)
	/* 成功: D0 = 1 を設定 */
	moveq   #1, %d0            | 成功: D0 = 1
	bra     PUT_BUF_Finish
PUT_BUF_Fail:
	moveq   #0, %d0
PUT_BUF_Finish:	
	movem.l   (%sp)+, %d2-%d5/%a1-%a3
	move.w    (%sp)+, %SR
	rts

****************
** OUTQ
****************
OUTQ:
	move.w  %SR, -(%sp)
	move.w  #0x2700, %SR
	movem.l   %d2-%d5/%a1-%a3, -(%sp)
GET_BUF:
	/* D3にQ_SIZEを準備 (MULU用) */
	move.w  #Q_SIZE, %d3
	/* D2 = キュー #D0 のオフセット計算 ( D0 * Q_SIZE) */
	move.l  %d0, %d2           | D2 = キュー番号
	mulu.w  %d3, %d2           | D2 = D2 * Q_SIZE
	/* A1 = キュー #D1 の先頭アドレス (topx) */
	movea.l #Q_BASE, %a1
	adda.l  %d2,%a1            | A1 = Q_BASE + オフセット (ベースアドレス)
	/* sのロードと空きチェック */
	move.w  s(%a1), %d4
	cmpi.w  #0, %d4            | s=0なら空き（読み込み不可）
	beq     GET_BUF_Fail
	/* データ読み込み */
	movea.l out(%a1),%a2       | A2 = outx
	move.b  (%a2)+,%d1         | (A2)のデータをD1へ、A2をインクリメント
	/* ラップアラウンドチェック */
	lea.l   bottom(%a1),%a3
	cmpa.l  %a3, %a2             
	bls     GET_BUF_STEP       | A2 <= A3 ならOK
	/* ラップアラウンド処理: A2 = topx (A1) */
	movea.l %a1, %a2         
GET_BUF_STEP:
	move.l  %a2,out(%a1)
	/* s のデクリメントと更新 */
	subq.w  #1, %d4
	move.w  %d4, s(%a1)
	/* 成功: D0 = 1 を設定 */
	moveq   #1, %d0            | 成功: D0 = 1
	bra     GET_BUF_Finish
GET_BUF_Fail:
	moveq   #0, %d0
GET_BUF_Finish:
	movem.l (%sp)+,%d2-%d5/%a1-%a3
	move.w  (%sp)+, %SR
	rts

/******************************************************/
/* システムコールライブラリ
/******************************************************/
****************************************
** PUTSTRING データを送信キューに転送
** 入力: %d1 (ch = 0)
**       %d2 (p = データの先頭アドレス)
**       %d3 (size = データのバイト数)
** 出力: なし
** %d1: chの入力、INQの入力
** %d2: sz = 実際に転送したデータ数
** %d3: size	
** %a0: データのアドレス
****************************************	
PUTSTRING:
	movem.l	%d1-%d3/%a0, -(%sp) | レジスタの退避
	cmp	#0, %d1
	bne	PS_END
	move.l	%d2, %a0            | a0 = i = address
	move.l	#0, %d2             | d2 = sz
	cmp	#0, %d3             | size == 0 => SET
	beq	PS_SET
PS_LOOP:
	cmp	%d2, %d3            | sz = size => UNMASK
	beq	PS_UNMASK
	move.l	#1, %d0             | 送信キューを選択
	move.b	(%a0), %d1          | data = p[sz]
	jsr	INQ
	cmp	#0, %d0     | INQが失敗 => UNMASK
	beq	PS_UNMASK
	addq	#1, %d2     | sz++
	addq	#1, %a0     | p++
	bra	PS_LOOP
PS_UNMASK:
	move.w	#0xe10c, USTCNT1    | 送信割り込みを許可
PS_SET:
	move.l	%d2, %d0
PS_END:
	movem.l	(%sp)+, %d1-%d3/%a0 | レジスタの回復
	rts

**************************************************
** GETSTRING
** 機能: 受信キューから指定サイズのデータを取り出し、メモリにコピーする
** 入力: ch (%D1.L), p (%D2.L), size (%D3.L)
**戻り値: sz (%D0.L)
**************************************************
GETSTRING:
	movem.l  %d1-%d4/%a1, -(%sp)
	cmp.l   #0, %d1
	bne     GETSTRING_Finish

	/* 初期化処理 */
    moveq   #0, %d4    | d0をszとし#0で初期化
	movea.l   %d2, %a1   | iを%a1とし，p -> i

GETSTRING_Loop:	
	cmp.l   %d3, %d4
	beq     GETSTRING_STEP  | size == sz なら終了

	/* OUTQ呼び出し準備 */
	moveq   #0, %d0  | キュー番号を#0に
	jsr     OUTQ     | dataが%d1 に格納されて帰ってくるはず
	cmp.l   #0, %d0  | 戻り値は#0(失敗)? 
	beq     GETSTRING_STEP
	addq    #1, %d4     | sz++ */
	move.b  %d1,(%a1)+  | i 番地に data をコピーし，i++

	bra     GETSTRING_Loop
	
	
GETSTRING_STEP:
	move.l  %d4, %d0   /* sz -> %D0.L */
	
	
GETSTRING_Finish:	
        movem.l  (%sp)+, %d1-%d4/%a1
	rts

/******************************************************/
/* 割り込み処理ルーチン
/******************************************************/
***************************************
** INTERPUT 送信キューからUTX1に転送
** 入力: %d1 (ch = 0)
** 出力: なし	
** %d0: キューの選択、OUTQの返り値
** %d1: chの入力、dataの格納場所
***************************************
INTERPUT:
	/* (走行レベルの退避は必要)？ */
	movem.l	%d0-%d1, -(%sp) | レジスタの退避
	move.w	#0x2700, %SR    | 割り込み禁止
	cmp	#0, %d1         | ch != 0 => 何もせず復帰
	bne	Interput_END
	move.l	#1, %d0         | 送信キューを選択
	jsr	OUTQ            | 送信キューから1biteを%d1 に取り出す
	cmp	#0, %d0         | OUTQが失敗 => 送信割り込みをマスク(禁止)
	beq	Interput_MASK
	/* 成功ならレジスタに転送 */
	add	#0x0800, %d1    | 0x0?00 = 0b_0000_??00_0000_0000
                                | 推奨値 = 0x0800
                                | 上位8bit分のヘッダ付与
	move.w	%d1, UTX1       | UTX1にデータを転送
	bra	Interput_END
Interput_MASK:
	move.w	#0xe108, USTCNT1    | 0xe108 = 0b0010_0001_0000_1000
Interput_END:
	movem.l	(%sp)+, %d0-%d1     | レジスタの回復
	rts

***************************************
** INTERGET
** 機能: 受信データを受信キューに格納する
** 入力: ch (%D1.L), data (%D2.B)
** 戻り値: なし
***************************************
INTERGET:
	movem   %d0-%d2, -(%sp)
	cmp.l   #0, %d1
	bne     INTERGET_Finish    | chが0以外なら終了

	/* INQ呼び出し準備 */
	move.l  #0, %d0
	move.b  %d2, %d1
	/*
	cmpi.l #0x31, %d1
	beq flush
	bra other
flush:
	move.b #0x31, LED0
other:*/
	jsr     INQ
INTERGET_Finish:
	movem   (%sp)+, %d0-%d2
	rts
	
***************************************************************
* タイマ制御部
*   - RESET_TIMER
*   - SET_TIMER
*   - CALL_RP
*   - timer_interface  (実際の割り込みハンドラ)
*
* 注意:
*   このファイルが単体で実行開始されたときに、いきなり
*   割り込みハンドラが走ってスタックを書きにいかないように、
*   先頭に安全なエントリ(start)を置いている。
***************************************************************
***************************************************************
* BSS : 割り込み時に呼ぶ関数ポインタ
***************************************************************
	.section .bss
	.even
task_p:	.ds.l 1		| SET_TIMER でここに呼び出し先を入れる

***************************************************************
* TEXT
***************************************************************
	.section .text
	.even

***************************************************************
* timer_start : このファイルが 0x400 からそのまま実行されたとき用の安全エントリ
*         何もしないでループするだけにしておく
*         （本来は別の step から呼ばれるユーティリティだから）
***************************************************************
timer_start:
	bra	timer_start   | とりあえずここで待機しておく

***************************************************************
* RESET_TIMER
*   タイマ停止＋IRQ禁止
***************************************************************
RESET_TIMER:
	move.w	#0x0004, TCTL1		| restart, IRQ disable, SYSCLK/16, timer stop
	rts

***************************************************************
* SET_TIMER(t, p)
*   %d1.w = 比較値 (t * 0.1ms)
*   %d2.l = 割り込みで呼ぶルーチンのアドレス
***************************************************************
SET_TIMER:
	movem.l	%d1-%d2, -(%sp)
	move.l	%d2, task_p
	move.w	#0x00CE, TPRER1
	move.w	%d1, TCMP1
	move.w	#0x0015, TCTL1
	movem.l	(%sp)+, %d1-%d2
	rts

***************************************************************
* CALL_RP
*   task_p に入っているアドレスを呼ぶだけ
***************************************************************
CALL_RP:
	movem.l	%a0, -(%sp)
	movea.l	task_p, %a0
	jsr	(%a0)
	movem.l	(%sp)+, %a0
	rts

/******************************************************/
/* 各種インターフェース
/******************************************************/
**************************
** 送受信割り込みインターフェース
**************************
UART_INTERFACE:
	movem.l %d0-%d7/%a0-%a6,-(%sp)
	/*受信割り込みか？*/
	move.w  URX1, %d3      |受信レジスタの内容を読み込む
	move.b  %d3, %d2       |下位8ビットをコピー
	btst    #13, %d3       |bit13が1(FIFOにデータがある)かどうかをテスト
	bne     CALL_INTERGET  |1なら分岐
	/*送信割り込みか？*/
	move.w  UTX1,%d0       |送信レジスタの内容を読み込む
	btst    #15, %d0       |bit15が1(FIFOが空)かどうかをテスト
	bne     CALL_INTERPUT  |もし1なら分岐
	movem.l (%sp)+, %d0-%d7/%a0-%a6
	rte
CALL_INTERGET:
	moveq   #0, %d1
	*move.b #0x31, LED0
	jsr     INTERGET
	movem.l (%sp)+, %d0-%d7/%a0-%a6
	rte
CALL_INTERPUT:
	moveq   #0, %d1
	jsr     INTERPUT
	movem.l (%sp)+, %d0-%d7/%a0-%a6
	rte
	
**************************
** タイマ用割り込みインターフェース
**************************
TIMER_INTERFACE:
	movem.l %d0-%d7/%a0-%a6,-(%sp)
	/* */
	move.w  TSTAT1, %d0         | タイマ1ステータスレジスタの内容を読み込む
	btst    #0,     %d0         | bit0が1かどうかをテスト
	beq     TIMER_INTERFACE_END | もし0なら終了
	move.w  #0,     TSTAT1      | 0でクリア
	jsr     CALL_RP
TIMER_INTERFACE_END:
	movem.l (%sp)+, %d0-%d7/%a0-%a6
	rte
	
**************************
** システムコールインターフェース
**************************
SYSTEM_CALL_INTERFACE:
	movem.l %d1-%d7/%a0-%a6,-(%sp)
	cmpi.l  #1, %d0
	beq     CALL_GETSTRING
	cmpi.l  #2, %d0
	beq     CALL_PUTSTRING
	cmpi.l  #3, %d0
	beq     CALL_RESET_TIMER
	cmpi.l  #4, %d0
	beq     CALL_SET_TIMER
	bra     SYS_INTERFACE_END   | 例外処理
CALL_GETSTRING:
	jsr     GETSTRING
	bra     SYS_INTERFACE_END
CALL_PUTSTRING:
	jsr     PUTSTRING
	bra     SYS_INTERFACE_END
CALL_RESET_TIMER:
	jsr     RESET_TIMER
	bra     SYS_INTERFACE_END
CALL_SET_TIMER:
	jsr     SET_TIMER
	bra     SYS_INTERFACE_END
SYS_INTERFACE_END:	
	movem.l (%sp)+, %d1-%d7/%a0-%a6
	rte
***************
** システムコール番号
***************
.equ SYSCALL_NUM_GETSTRING,   1
.equ SYSCALL_NUM_PUTSTRING,   2
.equ SYSCALL_NUM_RESET_TIMER, 3
.equ SYSCALL_NUM_SET_TIMER,   4
****************************************************************
*** 初期値の無いデータ領域
****************************************************************
.section .bss
BUF:
.ds.b 256    | BUF[256]
.even
USR_STK:
.ds.b 0x4000 | ユーザスタック領域
.even
USR_STK_TOP: | ユーザスタック領域の最後尾
	
