//---------------------------------------------------------------------------
//
//	X-UNIT
//	Programable General Extention Board for X68K
//
//	Powered by XM6 TypeG Technology.
//	Copyright (C) 2019-2000 GIMONS
//	[ ���C�� ]
//
//---------------------------------------------------------------------------

#include "rpi-SmartStart.h"
#include "emb-stdio.h"
#include "windows.h"

//---------------------------------------------------------------------------
//
//	�萔�錾(GIC)
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
//	�萔��`(MMU)
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
//	MMU�ϊ��e�[�u��
//
//---------------------------------------------------------------------------
static RegType_t
	__attribute__((aligned(TLB_ALIGNMENT)))
		pagetable[NUM_PGTABLE_ENTRIES] = { 0 };

//---------------------------------------------------------------------------
//
//	MMU�ϊ��e�[�u���\�z
//
//---------------------------------------------------------------------------
int SetupPageTable()
{
	int i;
	uint32_t msg[5] = { 0 };

	// VC�̊J�n�A�h���X���擾
	if (mailbox_tag_message(
		&msg[0], 5, MAILBOX_TAG_GET_VC_MEMORY, 8, 8, 0, 0) == 0) {
		return -1;
	}

	// VC�̊J�n�A�h���X�܂ł̓m�[�}���L���b�V���ݒ�
	msg[3] /= LEVEL1_BLOCKSIZE;
	for (i = 0; i < msg[3]; i++) 
	{
		pagetable[i] = (LEVEL1_BLOCKSIZE * i) | MT_NORMAL;
	}

	// �c��̓X�g�����O�I�[�_�[�Ńm�[�L���b�V��
	for (; i < NUM_PGTABLE_ENTRIES; i++) {
		pagetable[i] = (LEVEL1_BLOCKSIZE * i) | MT_DEVICE_NS;
	}

	return 0;
}

//---------------------------------------------------------------------------
//
//	MMU�L��
//
//---------------------------------------------------------------------------
void EnableMMU()
{
	enable_mmu_tables(pagetable);
}

//---------------------------------------------------------------------------
//
//	GICD������
//
//---------------------------------------------------------------------------
void InitGICD()
{
	int i;
	volatile uint32_t *gicd;

	// RPI4�̂�
	if (RPi_IO_Base_Addr != 0xfe000000) {
		return;
	}

	// GICD�̃x�[�X�A�h���X
	gicd = GICD_BASEADDR;

	// GIC����
	gicd[GICD_CTLR] = 0;

	// ���荞�݃N���A
	for (i = 0; i < 8; i++) {
		// ���荞�݃C�l�[�u���N���A
		gicd[GICD_ICENABLER0 + i] = 0xffffffff;
		// ���荞�݃y���f�B���O�N���A
		gicd[GICD_ICPENDR0 + i] = 0xffffffff;
		// ���荞�݃A�N�e�B�u�N���A
		gicd[GICD_ICACTIVER0 + i] = 0xffffffff;
	}

	// ���荞�ݗD��x
	for (i = 0; i < 64; i++) {
		gicd[GICD_IPRIORITYR0 + i] = 0xa0a0a0a0;
	}

	// ���荞�݃^�[�Q�b�g���R�A0�ɐݒ�
	for (i = 0; i < 64; i++) {
		gicd[GICD_ITARGETSR0 + i] = 0x01010101;
	}

	// ���荞�݂����x���g���K�ɐݒ�
	for (i = 0; i < 64; i++) {
		gicd[GICD_ICFGR0 + i] = 0;
	}

	// GIC�L��
	gicd[GICD_CTLR] = 1;
}

//---------------------------------------------------------------------------
//
//	GICC������
//
//---------------------------------------------------------------------------
void InitGICC()
{
	volatile uint32_t *gicc;

	// RPI4�̂�
	if (RPi_IO_Base_Addr != 0xfe000000) {
		return;
	}

	// GICC�̃x�[�X�A�h���X
	gicc = GICC_BASEADDR;

	// �R�A��CPU�C���^�[�t�F�X��L���ɂ���
	gicc[GICC_PMR] = 0xf0;
	gicc[GICC_CTLR] = 1;
}

//---------------------------------------------------------------------------
//
//	�e�R�A�̏�������i��
//
//---------------------------------------------------------------------------
static volatile int nSetup = 0;
void CoreSetup()
{
	// MMU������
	EnableMMU();

	// GICC������
	InitGICC();

	// �Z�b�g�A�b�v����
	nSetup++;
}

//---------------------------------------------------------------------------
//
//	RaSCSI���C��
//
//---------------------------------------------------------------------------
void startrascsi(void);

//---------------------------------------------------------------------------
//
//	���C��
//
//---------------------------------------------------------------------------
void main(void)
{
	// SmartStart������
	Init_EmbStdio(WriteText);
	PiConsole_Init(0, 0, 0, printf);
	displaySmartStart(printf);
	ARM_setmaxspeed(printf);

	// MMU�ϊ��e�[�u��������
	SetupPageTable();

	// GICD������
	InitGICD();

	// �R�A0�Z�b�g�A�b�v
	CoreSetup();

	printf("\n");

	// RaSCSI�X�^�[�g
	startrascsi();
}
