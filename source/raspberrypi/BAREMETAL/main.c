//---------------------------------------------------------------------------
//
//	X-UNIT
//	Programable General Extention Board for X68K
//
//	Powered by XM6 TypeG Technology.
//	Copyright (C) 2019-2000 GIMONS
//	[ メイン ]
//
//---------------------------------------------------------------------------

#include "rpi-SmartStart.h"
#include "emb-stdio.h"
#include "windows.h"

//---------------------------------------------------------------------------
//
//	定数宣言(GIC)
//
//---------------------------------------------------------------------------
#define GICD_BASEADDR		(uint32_t *)(0xFF841000)
#define GICC_BASEADDR		(uint32_t *)(0xFF842000)
#define GICD_CTLR			0x000
#define GICD_IGROUPR0		0x020
#define GICD_ISENABLER0		0x040
#define GICD_ICENABLER0		0x060
#define GICD_ISPENDR0		0x080
#define GICD_ICPENDR0		0x0A0
#define GICD_ISACTIVER0		0x0C0
#define GICD_ICACTIVER0		0x0E0
#define GICD_IPRIORITYR0	0x100
#define GICD_ITARGETSR0		0x200
#define GICD_ICFGR0			0x300
#define GICD_SGIR			0x3C0
#define GICC_CTLR			0x000
#define GICC_PMR			0x001
#define GICC_IAR			0x003
#define GICC_EOIR			0x004

//---------------------------------------------------------------------------
//
//	定数定義(MMU)
//
//---------------------------------------------------------------------------
#define NUM_PGTABLE_ENTRIES	4096
#define LEVEL1_BLOCKSIZE	(1 << 20)
#define TLB_ALIGNMENT		16384
#define MT_DEVICE_NS		0x10412
#define MT_DEVICE			0x10416
#define MT_NORMAL			0x1040E
#define MT_NORMAL_NS		0x1040A
#define MT_NORMAL_XN		0x1041E

//---------------------------------------------------------------------------
//
//	MMU変換テーブル
//
//---------------------------------------------------------------------------
static RegType_t
	__attribute__((aligned(TLB_ALIGNMENT)))
		pagetable[NUM_PGTABLE_ENTRIES] = { 0 };

//---------------------------------------------------------------------------
//
//	MMU変換テーブル構築
//
//---------------------------------------------------------------------------
int SetupPageTable()
{
	int i;
	uint32_t msg[5] = { 0 };

	// VCの開始アドレスを取得
	if (mailbox_tag_message(
		&msg[0], 5, MAILBOX_TAG_GET_VC_MEMORY, 8, 8, 0, 0) == 0) {
		return -1;
	}

	// VCの開始アドレスまではノーマルキャッシュ設定
	msg[3] /= LEVEL1_BLOCKSIZE;
	for (i = 0; i < msg[3]; i++) 
	{
		pagetable[i] = (LEVEL1_BLOCKSIZE * i) | MT_NORMAL;
	}

	// 残りはストロングオーダーでノーキャッシュ
	for (; i < NUM_PGTABLE_ENTRIES; i++) {
		pagetable[i] = (LEVEL1_BLOCKSIZE * i) | MT_DEVICE_NS;
	}

	return 0;
}

//---------------------------------------------------------------------------
//
//	MMU有効
//
//---------------------------------------------------------------------------
void EnableMMU()
{
	enable_mmu_tables(pagetable);
}

//---------------------------------------------------------------------------
//
//	GICD初期化
//
//---------------------------------------------------------------------------
void InitGICD()
{
	int i;
	volatile uint32_t *gicd;

	// RPI4のみ
	if (RPi_IO_Base_Addr != 0xfe000000) {
		return;
	}

	// GICDのベースアドレス
	gicd = GICD_BASEADDR;

	// GIC無効
	gicd[GICD_CTLR] = 0;

	// 割り込みクリア
	for (i = 0; i < 8; i++) {
		// 割り込みイネーブルクリア
		gicd[GICD_ICENABLER0 + i] = 0xffffffff;
		// 割り込みペンディングクリア
		gicd[GICD_ICPENDR0 + i] = 0xffffffff;
		// 割り込みアクティブクリア
		gicd[GICD_ICACTIVER0 + i] = 0xffffffff;
	}

	// 割り込み優先度
	for (i = 0; i < 64; i++) {
		gicd[GICD_IPRIORITYR0 + i] = 0xa0a0a0a0;
	}

	// 割り込みターゲットをコア0に設定
	for (i = 0; i < 64; i++) {
		gicd[GICD_ITARGETSR0 + i] = 0x01010101;
	}

	// 割り込みをレベルトリガに設定
	for (i = 0; i < 64; i++) {
		gicd[GICD_ICFGR0 + i] = 0;
	}

	// GIC有効
	gicd[GICD_CTLR] = 1;
}

//---------------------------------------------------------------------------
//
//	GICC初期化
//
//---------------------------------------------------------------------------
void InitGICC()
{
	volatile uint32_t *gicc;

	// RPI4のみ
	if (RPi_IO_Base_Addr != 0xfe000000) {
		return;
	}

	// GICCのベースアドレス
	gicc = GICC_BASEADDR;

	// コアのCPUインターフェスを有効にする
	gicc[GICC_PMR] = 0xf0;
	gicc[GICC_CTLR] = 1;
}

//---------------------------------------------------------------------------
//
//	各コアの初期化二段目
//
//---------------------------------------------------------------------------
static volatile int nSetup = 0;
void CoreSetup()
{
	// MMU初期化
	EnableMMU();

	// GICC初期化
	InitGICC();

	// セットアップ完了
	nSetup++;
}

//---------------------------------------------------------------------------
//
//	RaSCSIメイン
//
//---------------------------------------------------------------------------
void startrascsi(void);

//---------------------------------------------------------------------------
//
//	メイン
//
//---------------------------------------------------------------------------
void main(void)
{
	// SmartStart初期化
	Init_EmbStdio(WriteText);
	PiConsole_Init(0, 0, 0, printf);
	displaySmartStart(printf);
	ARM_setmaxspeed(printf);

	// MMU変換テーブル初期化
	SetupPageTable();

	// GICD初期化
	InitGICD();

	// コア0セットアップ
	CoreSetup();

	printf("\n");

	// RaSCSIスタート
	startrascsi();
}
