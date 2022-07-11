//---------------------------------------------------------------------------
//
//	SCSI Target Emulator RaSCSI (*^..^*)
//	for Raspberry Pi
//	Powered by XM6 TypeG Technology.
//
//	Copyright (C) 2016-2021 GIMONS(Twitter:@kugimoto0715)
//	Imported NetBSD support and some optimisation patch by Rin Okuyama.
//
//	[ GPIO-SCSIバス ]
//
//---------------------------------------------------------------------------

#include "os.h"
#include "rascsi.h"
#include "disk.h"
#include "gpiobus.h"

#ifndef BAREMETAL
#ifdef __linux__
//---------------------------------------------------------------------------
//
//	imported from bcm_host.c
//
//---------------------------------------------------------------------------
static DWORD get_dt_ranges(const char *filename, DWORD offset)
{
	DWORD address;
	FILE *fp;
	BYTE buf[4];

	address = ~0;
	fp = fopen(filename, "rb");
	if (fp) {
		fseek(fp, offset, SEEK_SET);
		if (fread(buf, 1, sizeof buf, fp) == sizeof buf) {
			address =
				buf[0] << 24 | buf[1] << 16 | buf[2] << 8 | buf[3] << 0;
		}
		fclose(fp);
	}
	return address;
}

DWORD bcm_host_get_peripheral_address(void)
{
	DWORD address;

	address = get_dt_ranges("/proc/device-tree/soc/ranges", 4);
	if (address == 0) {
		address = get_dt_ranges("/proc/device-tree/soc/ranges", 8);
	}
	address = (address == (DWORD)~0) ? 0x20000000 : address;
#if 0
	printf("Peripheral address : 0x%lx\n", address);
#endif
	return address;
}
#endif // __linux__

#ifdef __NetBSD__
// Raspberry Piシリーズを仮定してCPUからアドレスを推定
DWORD bcm_host_get_peripheral_address(void)
{
	char buf[1024];
	size_t len = sizeof(buf);
	DWORD address;
	
	if (sysctlbyname("hw.model", buf, &len, NULL, 0) ||
	    strstr(buf, "ARM1176JZ-S") != buf) {
		// CPUモデルの取得に失敗 || BCM2835ではない
		// BCM283[67]のアドレスを使用
		address = 0x3f000000;
	} else {
		// BCM2835のアドレスを使用
		address = 0x20000000;
	}
	printf("Peripheral address : 0x%lx\n", address);
	return address;
}
#endif	// __NetBSD__
#endif	// BAREMETAL

#ifdef BAREMETAL
//---------------------------------------------------------------------------
//
//	RPI基本情報
//
//---------------------------------------------------------------------------
extern "C" {
extern uint32_t RPi_IO_Base_Addr;
extern uint32_t ARM_getcorespeed();
}

//---------------------------------------------------------------------------
//
//	割り込み制御関数
//
//---------------------------------------------------------------------------
extern "C" {
extern uintptr_t setIrqFuncAddress (void(*ARMaddress)(void));
extern void EnableInterrupts (void);
extern void DisableInterrupts (void);
extern void WaitForInterrupts (void);
}

//---------------------------------------------------------------------------
//
//	割り込みハンドラ
//
//---------------------------------------------------------------------------
static GPIOBUS *self;
extern "C"
void IrqHandler()
{
	// 割り込みクリア
	self->ClearSelectEvent();
}
#endif	// BAREMETAL

//---------------------------------------------------------------------------
//
//	コンストラクタ
//
//---------------------------------------------------------------------------
GPIOBUS::GPIOBUS()
{
#ifdef BAREMETAL
	self = this;
#endif	// BAREMETAL
}

//---------------------------------------------------------------------------
//
//	デストラクタ
//
//---------------------------------------------------------------------------
GPIOBUS::~GPIOBUS()
{
}

//---------------------------------------------------------------------------
//
//	初期化
//
//---------------------------------------------------------------------------
BOOL FASTCALL GPIOBUS::Init()
{
	void *map;
	int i;
	int j;
	int pullmode;
#ifndef BAREMETAL
	int fd;
	struct epoll_event ev[2];
#endif	// !BAREMETAL

	// 動作モードのデフォルトはターゲット
	actmode = mode_e::TARGET;

#ifdef BAREMETAL
	// ベースアドレスの取得
	baseaddr = RPi_IO_Base_Addr;
	map = (void*)baseaddr;
#else
	// ベースアドレスの取得
	baseaddr = (DWORD)bcm_host_get_peripheral_address();

	// /dev/memオープン
	fd = open("/dev/mem", O_RDWR | O_SYNC);
	if (fd == -1) {
		return FALSE;
	}

	// ペリフェラルリージョンのメモリをマップ
	map = mmap(NULL, 0x1000100,
		PROT_READ | PROT_WRITE, MAP_SHARED, fd, baseaddr);
	if (map == MAP_FAILED) {
		close(fd);
		return FALSE;
	}
#endif	// BAREMETAL

	// ベースアドレスからラズパイのタイプを決定
	if (baseaddr == 0xfe000000) {
		rpitype = 4;
	} else if (baseaddr == 0x3f000000) {
		rpitype = 2;
	} else {
		rpitype = 1;
	}

	// GPIO
	gpio = (DWORD *)map;
	gpio += GPIO_OFFSET / sizeof(DWORD);
	level = &gpio[GPIO_LEV_0];

	// PADS
	pads = (DWORD *)map;
	pads += PADS_OFFSET / sizeof(DWORD);

	// クロックマネージャ
	cm = (DWORD *)map;
	cm += CM_OFFSET / sizeof(DWORD);

	// システムタイマ
	SysTimer::Init(
		(DWORD *)map + SYST_OFFSET / sizeof(DWORD),
		(DWORD *)map + ARMT_OFFSET / sizeof(DWORD));

	// 割り込みコントローラ
	irpctl = (DWORD *)map;
	irpctl += IRPT_OFFSET / sizeof(DWORD);

#ifndef BAREMETAL
	// Quad-A7 control
	qa7regs = (DWORD *)map;
	qa7regs += QA7_OFFSET / sizeof(DWORD);
#endif	// !BAREMETAL

#ifdef BAREMETAL
	// GICのメモリをマップ
	if (rpitype == 4) {
		map = (void*)ARM_GICD_BASE;
		gicd = (DWORD *)map;
		map = (void*)ARM_GICC_BASE;
		gicc = (DWORD *)map;
	} else {
		gicd = NULL;
		gicc = NULL;
	}
#else
	// GICのメモリをマップ
	if (rpitype == 4) {
		map = mmap(NULL, 8192,
			PROT_READ | PROT_WRITE, MAP_SHARED, fd, ARM_GICD_BASE);
		if (map == MAP_FAILED) {
			close(fd);
			return FALSE;
		}
		gicd = (DWORD *)map;
		gicc = (DWORD *)map;
		gicc += (ARM_GICC_BASE - ARM_GICD_BASE) / sizeof(DWORD);
	} else {
		gicd = NULL;
		gicc = NULL;
	}
	close(fd);
#endif	// BAREMETAL

	// Drive Strengthを16mAに設定
	DrvConfig(7);

	// プルアップ/プルダウンを設定
#if SIGNAL_CONTROL_MODE == 0
	pullmode = GPIO_PULLUP;
#elif SIGNAL_CONTROL_MODE == 1
	pullmode = GPIO_PULLUP;
#else
	pullmode = GPIO_PULLDOWN;
#endif

	// 全信号初期化
	for (i = 0; SignalTable[i] >= 0; i++) {
		j = SignalTable[i];
		PinSetSignal(j, FALSE);
		PinConfig(j, GPIO_INPUT);
		PullConfig(j, pullmode);
	}

	// 制御信号を設定
	PinSetSignal(PIN_ACT, FALSE);
	PinSetSignal(PIN_TAD, FALSE);
	PinSetSignal(PIN_IND, FALSE);
	PinSetSignal(PIN_DTD, FALSE);
	PinConfig(PIN_ACT, GPIO_OUTPUT);
	PinConfig(PIN_TAD, GPIO_OUTPUT);
	PinConfig(PIN_IND, GPIO_OUTPUT);
	PinConfig(PIN_DTD, GPIO_OUTPUT);

	// ENABLE信号を設定
	PinSetSignal(PIN_ENB, ENB_OFF);
	PinConfig(PIN_ENB, GPIO_OUTPUT);

	// GPFSELバックアップ
	gpfsel[0] = gpio[GPIO_FSEL_0];
	gpfsel[1] = gpio[GPIO_FSEL_1];
	gpfsel[2] = gpio[GPIO_FSEL_2];
	gpfsel[3] = gpio[GPIO_FSEL_3];
	gpfsel[4] = gpio[GPIO_FSEL_4];
	gpfsel[5] = gpio[GPIO_FSEL_5];

	// SEL信号割り込み初期化
#ifndef BAREMETAL
	// GPIOチップオープン
	fd = open("/dev/gpiochip0", 0);
	if (fd == -1) {
		return FALSE;
	}

	// イベント要求設定
	strcpy(selevreq.consumer_label, "RaSCSI SELECT");
	selevreq.lineoffset = PIN_SEL;
	selevreq.handleflags = GPIOHANDLE_REQUEST_INPUT;
#if SIGNAL_CONTROL_MODE < 2
	selevreq.eventflags = GPIOEVENT_REQUEST_FALLING_EDGE;
#else
	selevreq.eventflags = GPIOEVENT_REQUEST_RISING_EDGE;
#endif	// SIGNAL_CONTROL_MODE

	strcpy(rstevreq.consumer_label, "RaSCSI RESET");
	rstevreq.lineoffset = PIN_RST;
	rstevreq.handleflags = GPIOHANDLE_REQUEST_INPUT;
#if SIGNAL_CONTROL_MODE < 2
	rstevreq.eventflags = GPIOEVENT_REQUEST_FALLING_EDGE;
#else
	rstevreq.eventflags = GPIOEVENT_REQUEST_RISING_EDGE;
#endif	// SIGNAL_CONTROL_MODE

	// イベント要求取得
	if (ioctl(fd, GPIO_GET_LINEEVENT_IOCTL, &selevreq) == -1) {
		close(fd);
		return FALSE;
	}

	if (ioctl(fd, GPIO_GET_LINEEVENT_IOCTL, &rstevreq) == -1) {
		close(fd);
		return FALSE;
	}

	// GPIOチップクローズ
	close(fd);

	// epoll初期化
	epfd = epoll_create(2);
	memset(&ev[0], 0, sizeof(struct epoll_event));
	ev[0].events = EPOLLIN | EPOLLPRI;
	ev[0].data.fd = selevreq.fd;
	epoll_ctl(epfd, EPOLL_CTL_ADD, selevreq.fd, &ev[0]);

	memset(&ev[1], 0, sizeof(struct epoll_event));
	ev[1].events = EPOLLIN | EPOLLPRI;
	ev[1].data.fd = rstevreq.fd;
	epoll_ctl(epfd, EPOLL_CTL_ADD, selevreq.fd, &ev[1]);

	// リアルタイムクラスのスケジューラパラメータ設定
	rttime[0] = '\0';
	fd = open(PATH_RTTIME, O_RDWR | O_SYNC);
	if (fd >= 0) {
		read(fd, rttime, 20);
		close(fd);

		// CPU時間制限を解除
		fd = open(PATH_RTTIME, O_RDWR | O_SYNC);
		if (fd >= 0) {
			write(fd, "-1", 2);
			close(fd);
		}
	}
#else
	// エッジ検出設定
#if SIGNAL_CONTROL_MODE == 2
	gpio[GPIO_AREN_0] = (1 << PIN_SEL) | (1 << PIN_RST);
#else
	gpio[GPIO_AFEN_0] = (1 << PIN_SEL) | (1 << PIN_RST);
#endif	// SIGNAL_CONTROL_MODE

	// イベントクリア
	gpio[GPIO_EDS_0] = (1 << PIN_SEL) | (1 << PIN_RST);

	// 割り込みハンドラ登録
	setIrqFuncAddress(IrqHandler);

	// GPIO割り込み設定
	if (rpitype == 4) {
		// 割り込み有効
		gicd[GICD_ISENABLER0 + (GIC_GPIO_IRQ / 32)] =
			1 << (GIC_GPIO_IRQ % 32);
	} else {
		// 割り込み有効
		irpctl[IRPT_ENB_IRQ_2] = (1 << (GPIO_IRQ % 32));
	}
#endif	// !BAREMETAL

	// ワークテーブル作成
	MakeTable();

	// 最後にENABLEをオン
	SetControl(PIN_ENB, ENB_ON);

	return TRUE;
}

//---------------------------------------------------------------------------
//
//	動作モード設定
//
//---------------------------------------------------------------------------
void FASTCALL GPIOBUS::SetMode(mode_e mode)
{
	// 動作モードの保存
	actmode = mode;
}

//---------------------------------------------------------------------------
//
//	クリーンアップ
//
//---------------------------------------------------------------------------
void FASTCALL GPIOBUS::Cleanup()
{
	int i;
#ifndef BAREMETAL
	int fd;
#endif	// !BAREMETAL
	int pin;

#ifndef BAREMETAL
	// SEL信号割り込み解放
	close(selevreq.fd);
	close(rstevreq.fd);

	// リアルタイムクラスのスケジューラパラメータ設定を戻す
	if (rttime[0] != '\0') {
		fd = open(PATH_RTTIME, O_RDWR | O_SYNC);
		if (fd >= 0) {
			write(fd, rttime, strlen(rttime));
			close(fd);
		}
	}
#endif	// !BAREMETAL

	// 制御信号を設定
	PinSetSignal(PIN_ENB, FALSE);
	PinSetSignal(PIN_ACT, FALSE);
	PinSetSignal(PIN_TAD, FALSE);
	PinSetSignal(PIN_IND, FALSE);
	PinSetSignal(PIN_DTD, FALSE);
	PinConfig(PIN_ACT, GPIO_INPUT);
	PinConfig(PIN_TAD, GPIO_INPUT);
	PinConfig(PIN_IND, GPIO_INPUT);
	PinConfig(PIN_DTD, GPIO_INPUT);

	// 全信号初期化
	for (i = 0; SignalTable[i] >= 0; i++) {
		pin = SignalTable[i];
		PinSetSignal(pin, FALSE);
		PinConfig(pin, GPIO_INPUT);
		PullConfig(pin, GPIO_PULLNONE);
	}

	// Drive Strengthを8mAに設定
	DrvConfig(3);
}

//---------------------------------------------------------------------------
//
//	リセット
//
//---------------------------------------------------------------------------
void FASTCALL GPIOBUS::Reset()
{
	int i;
	int j;

	// アクティブ信号をオフ
	SetControl(PIN_ACT, ACT_OFF);

	// 全信号をネゲートに設定
	for (i = 0;; i++) {
		j = SignalTable[i];
		if (j < 0) {
			break;
		}

		SetSignal(j, OFF);
	}

	if (actmode == TARGET) {
		// ターゲットモード

		// ターゲット信号を入力に設定
		SetControl(PIN_TAD, TAD_IN);
		SetMode(PIN_BSY, IN);
		SetMode(PIN_MSG, IN);
		SetMode(PIN_CD, IN);
		SetMode(PIN_REQ, IN);
		SetMode(PIN_IO, IN);

		// イニシエータ信号を入力に設定
		SetControl(PIN_IND, IND_IN);
		SetMode(PIN_SEL, IN);
		SetMode(PIN_ATN, IN);
		SetMode(PIN_ACK, IN);
		SetMode(PIN_RST, IN);

		// データバス信号を入力に設定
		SetControl(PIN_DTD, DTD_IN);
		SetMode(PIN_DT0, IN);
		SetMode(PIN_DT1, IN);
		SetMode(PIN_DT2, IN);
		SetMode(PIN_DT3, IN);
		SetMode(PIN_DT4, IN);
		SetMode(PIN_DT5, IN);
		SetMode(PIN_DT6, IN);
		SetMode(PIN_DT7, IN);
		SetMode(PIN_DP, IN);

#if USE_SYNC_TRANS == 1
		// GPCLK設定
		// PLLD = 6 3B:500 MHz 4:750 MHz
		// 5MHz出力設定
		i = rpitype == 4 ? (750 / 5) * 1 : (500 / 5) * 1;
		PinConfig(PIN_GPCLK, GPIO_ALT0);
		cm[CM_GPCTL] = CM_PASSWORD | CM_KILL | CM_SRC_PLLDPER;
		cm[CM_GPDIV] = CM_PASSWORD | CM_DIVI(i);
		cm[CM_GPCTL] = CM_PASSWORD | CM_ENAB | CM_SRC_PLLDPER;
#endif	// USE_SYNC_TRANS == 1
	} else {
		// イニシエータモード

		// ターゲット信号を入力に設定
		SetControl(PIN_TAD, TAD_IN);
		SetMode(PIN_BSY, IN);
		SetMode(PIN_MSG, IN);
		SetMode(PIN_CD, IN);
		SetMode(PIN_REQ, IN);
		SetMode(PIN_IO, IN);

		// イニシエータ信号を出力に設定
		SetControl(PIN_IND, IND_OUT);
		SetMode(PIN_SEL, OUT);
		SetMode(PIN_ATN, OUT);
		SetMode(PIN_ACK, OUT);
		SetMode(PIN_RST, OUT);

		// データバス信号を出力に設定
		SetControl(PIN_DTD, DTD_OUT);
		SetMode(PIN_DT0, OUT);
		SetMode(PIN_DT1, OUT);
		SetMode(PIN_DT2, OUT);
		SetMode(PIN_DT3, OUT);
		SetMode(PIN_DT4, OUT);
		SetMode(PIN_DT5, OUT);
		SetMode(PIN_DT6, OUT);
		SetMode(PIN_DT7, OUT);
		SetMode(PIN_DP, OUT);
	}

	// 全信号初期化
	signals = 0;
}

//---------------------------------------------------------------------------
//
//	データ信号方向切り替え
//
//---------------------------------------------------------------------------
void FASTCALL GPIOBUS::SetDataDirection(datadir_e dir)
{
	if (dir == DATA_DIR_IN) {
		SetControl(PIN_DTD, DTD_IN);
		SetDAT(0);
		SetMode(PIN_DT0, IN);
		SetMode(PIN_DT1, IN);
		SetMode(PIN_DT2, IN);
		SetMode(PIN_DT3, IN);
		SetMode(PIN_DT4, IN);
		SetMode(PIN_DT5, IN);
		SetMode(PIN_DT6, IN);
		SetMode(PIN_DT7, IN);
		SetMode(PIN_DP, IN);
	} else {
		SetControl(PIN_DTD, DTD_OUT);
		SetMode(PIN_DT0, OUT);
		SetMode(PIN_DT1, OUT);
		SetMode(PIN_DT2, OUT);
		SetMode(PIN_DT3, OUT);
		SetMode(PIN_DT4, OUT);
		SetMode(PIN_DT5, OUT);
		SetMode(PIN_DT6, OUT);
		SetMode(PIN_DT7, OUT);
		SetMode(PIN_DP, OUT);
	}
}

//---------------------------------------------------------------------------
//
//	バス信号取り込み
//
//---------------------------------------------------------------------------
DWORD FASTCALL GPIOBUS::Aquire() const
{
	signals = *level;

#if SIGNAL_CONTROL_MODE < 2
	// 負論理なら反転する(内部処理は正論理に統一)
	signals = ~signals;
#endif	// SIGNAL_CONTROL_MODE
	
	return signals;
}

//---------------------------------------------------------------------------
//
//	ENBシグナル設定
//
//---------------------------------------------------------------------------
void FASTCALL GPIOBUS::SetENB(BOOL ast)
{
	PinSetSignal(PIN_ENB, ast ? ENB_ON : ENB_OFF);
}

//---------------------------------------------------------------------------
//
//	BSYシグナル取得
//
//---------------------------------------------------------------------------
BOOL FASTCALL GPIOBUS::GetBSY() const
{
	return GetSignal(PIN_BSY);
}

//---------------------------------------------------------------------------
//
//	BSYシグナル設定
//
//---------------------------------------------------------------------------
void FASTCALL GPIOBUS::SetBSY(BOOL ast)
{
	// BSY信号を設定
	SetSignal(PIN_BSY, ast);

	if (actmode == TARGET) {
		if (ast) {
			// アクティブ信号をオン
			SetControl(PIN_ACT, ACT_ON);

			// ターゲット信号を出力に設定
			SetControl(PIN_TAD, TAD_OUT);

			SetMode(PIN_BSY, OUT);
			SetMode(PIN_MSG, OUT);
			SetMode(PIN_CD, OUT);
			SetMode(PIN_REQ, OUT);
			SetMode(PIN_IO, OUT);
		} else {
			// アクティブ信号をオフ
			SetControl(PIN_ACT, ACT_OFF);

			// ターゲット信号を入力に設定
			SetControl(PIN_TAD, TAD_IN);

			SetMode(PIN_BSY, IN);
			SetMode(PIN_MSG, IN);
			SetMode(PIN_CD, IN);
			SetMode(PIN_REQ, IN);
			SetMode(PIN_IO, IN);
		}
	}
}

//---------------------------------------------------------------------------
//
//	SELシグナル取得
//
//---------------------------------------------------------------------------
BOOL FASTCALL GPIOBUS::GetSEL() const
{
	return GetSignal(PIN_SEL);
}

//---------------------------------------------------------------------------
//
//	SELシグナル設定
//
//---------------------------------------------------------------------------
void FASTCALL GPIOBUS::SetSEL(BOOL ast)
{
	if (actmode == INITIATOR && ast) {
		// アクティブ信号をオン
		SetControl(PIN_ACT, ACT_ON);
	}

	// SEL信号を設定
	SetSignal(PIN_SEL, ast);
}

//---------------------------------------------------------------------------
//
//	ATNシグナル取得
//
//---------------------------------------------------------------------------
BOOL FASTCALL GPIOBUS::GetATN() const
{
	return GetSignal(PIN_ATN);
}

//---------------------------------------------------------------------------
//
//	ATNシグナル設定
//
//---------------------------------------------------------------------------
void FASTCALL GPIOBUS::SetATN(BOOL ast)
{
	SetSignal(PIN_ATN, ast);
}

//---------------------------------------------------------------------------
//
//	ACKシグナル取得
//
//---------------------------------------------------------------------------
BOOL FASTCALL GPIOBUS::GetACK() const
{
	return GetSignal(PIN_ACK);
}

//---------------------------------------------------------------------------
//
//	ACKシグナル設定
//
//---------------------------------------------------------------------------
void FASTCALL GPIOBUS::SetACK(BOOL ast)
{
	SetSignal(PIN_ACK, ast);
}

//---------------------------------------------------------------------------
//
//	RSTシグナル取得
//
//---------------------------------------------------------------------------
BOOL FASTCALL GPIOBUS::GetRST() const
{
	return GetSignal(PIN_RST);
}

//---------------------------------------------------------------------------
//
//	RSTシグナル設定
//
//---------------------------------------------------------------------------
void FASTCALL GPIOBUS::SetRST(BOOL ast)
{
	SetSignal(PIN_RST, ast);
}

//---------------------------------------------------------------------------
//
//	MSGシグナル取得
//
//---------------------------------------------------------------------------
BOOL FASTCALL GPIOBUS::GetMSG() const
{
	return GetSignal(PIN_MSG);
}

//---------------------------------------------------------------------------
//
//	MSGシグナル設定
//
//---------------------------------------------------------------------------
void FASTCALL GPIOBUS::SetMSG(BOOL ast)
{
	SetSignal(PIN_MSG, ast);
}

//---------------------------------------------------------------------------
//
//	CDシグナル取得
//
//---------------------------------------------------------------------------
BOOL FASTCALL GPIOBUS::GetCD() const
{
	return GetSignal(PIN_CD);
}

//---------------------------------------------------------------------------
//
//	CDシグナル設定
//
//---------------------------------------------------------------------------
void FASTCALL GPIOBUS::SetCD(BOOL ast)
{
	SetSignal(PIN_CD, ast);
}

//---------------------------------------------------------------------------
//
//	IOシグナル取得
//
//---------------------------------------------------------------------------
BOOL FASTCALL GPIOBUS::GetIO() const
{
	return GetSignal(PIN_IO);
}

//---------------------------------------------------------------------------
//
//	IOシグナル設定
//
//---------------------------------------------------------------------------
void FASTCALL GPIOBUS::SetIO(BOOL ast)
{
	SetSignal(PIN_IO, ast);

	if (actmode == TARGET) {
		// IO信号によってデータの入出力方向を変更
		if (ast) {
			SetDataDirection(DATA_DIR_OUT);
		} else {
			SetDataDirection(DATA_DIR_IN);
		}
	}
}

//---------------------------------------------------------------------------
//
//	REQシグナル取得
//
//---------------------------------------------------------------------------
BOOL FASTCALL GPIOBUS::GetREQ() const
{
	return GetSignal(PIN_REQ);
}

//---------------------------------------------------------------------------
//
//	REQシグナル設定
//
//---------------------------------------------------------------------------
void FASTCALL GPIOBUS::SetREQ(BOOL ast)
{
	SetSignal(PIN_REQ, ast);
}

//---------------------------------------------------------------------------
//
//	データシグナル取得
//
//---------------------------------------------------------------------------
BYTE FASTCALL GPIOBUS::GetDAT() const
{
	DWORD data;

	data = signals;
	data =
		((data >> (PIN_DT0 - 0)) & (1 << 0)) |
		((data >> (PIN_DT1 - 1)) & (1 << 1)) |
		((data >> (PIN_DT2 - 2)) & (1 << 2)) |
		((data >> (PIN_DT3 - 3)) & (1 << 3)) |
		((data >> (PIN_DT4 - 4)) & (1 << 4)) |
		((data >> (PIN_DT5 - 5)) & (1 << 5)) |
		((data >> (PIN_DT6 - 6)) & (1 << 6)) |
		((data >> (PIN_DT7 - 7)) & (1 << 7));

	return (BYTE)data;
}

//---------------------------------------------------------------------------
//
//	データシグナル設定
//
//---------------------------------------------------------------------------
void FASTCALL GPIOBUS::SetDAT(BYTE dat)
{
	// ポートへ書き込み
#if SIGNAL_CONTROL_MODE == 0
	DWORD fsel;

	fsel = gpfsel[0];
	fsel &= tblDatMsk[0][dat];
	fsel |= tblDatSet[0][dat];
	if (fsel != gpfsel[0]) {
		gpfsel[0] = fsel;
		gpio[GPIO_FSEL_0] = fsel;
	}

	fsel = gpfsel[1];
	fsel &= tblDatMsk[1][dat];
	fsel |= tblDatSet[1][dat];
	if (fsel != gpfsel[1]) {
		gpfsel[1] = fsel;
		gpio[GPIO_FSEL_1] = fsel;
	}

	fsel = gpfsel[2];
	fsel &= tblDatMsk[2][dat];
	fsel |= tblDatSet[2][dat];
	if (fsel != gpfsel[2]) {
		gpfsel[2] = fsel;
		gpio[GPIO_FSEL_2] = fsel;
	}
#else
	gpio[GPIO_CLR_0] = tblDatMsk[dat];
	gpio[GPIO_SET_0] = tblDatSet[dat];
#endif	// SIGNAL_CONTROL_MODE

	// メモリバリア
	MemoryBarrier();
}

//---------------------------------------------------------------------------
//
//	データパリティシグナル取得
//
//---------------------------------------------------------------------------
BOOL FASTCALL GPIOBUS::GetDP() const
{
	return GetSignal(PIN_DP);
}

//---------------------------------------------------------------------------
//
//	コマンド受信ハンドシェイク
//
//---------------------------------------------------------------------------
int FASTCALL GPIOBUS::CommandHandShake(BYTE *buf)
{
	int i;
	BOOL ret;
	int count;

	// ターゲットモードのみ
	if (actmode != TARGET) {
		return 0;
	}

	// IRQ無効
	DisableIRQ();

	// 最初のコマンドバイトを取得
	i = 0;

	// REQアサート
	SetSignal(PIN_REQ, ON);

	// ACKアサート待ち
	ret = WaitSignal(PIN_ACK, ON);

	// 信号線が安定するまでウェイト
	DelayBits();

	// データ取得
	Aquire();
	*buf = GetDAT();

	// REQネゲート
	SetSignal(PIN_REQ, OFF);

	// ACKアサート待ちでタイムアウト
	if (!ret) {
		goto irq_enable_exit;
	}

	// ACKネゲート待ち
	ret = WaitSignal(PIN_ACK, OFF);

	// ACKネゲート待ちでタイムアウト
	if (!ret) {
		goto irq_enable_exit;
	}

	// コマンドが6バイトか10バイトか見分ける
	if (*buf >= 0x20 && *buf <= 0x7D) {
		count = 10;
	} else {
		count = 6;
	}

	// 次データへ
	buf++;

	for (i = 1; i < count; i++) {
		// REQアサート
		SetSignal(PIN_REQ, ON);

		// ACKアサート待ち
		ret = WaitSignal(PIN_ACK, ON);

		// 信号線が安定するまでウェイト
		DelayBits();

		// データ取得
		Aquire();
		*buf = GetDAT();

		// REQネゲート
		SetSignal(PIN_REQ, OFF);

		// ACKアサート待ちでタイムアウト
		if (!ret) {
			break;
		}

		// ACKネゲート待ち
		ret = WaitSignal(PIN_ACK, OFF);

		// ACKネゲート待ちでタイムアウト
		if (!ret) {
			break;
		}

		// 次データへ
		buf++;
	}

irq_enable_exit:
	// IRQ有効
	EnableIRQ();

	// 受信数を返却
	return i;
}

//---------------------------------------------------------------------------
//
//	一括データ送信ハンドシェイク
//
//---------------------------------------------------------------------------
int FASTCALL GPIOBUS::SendHandShake(BYTE *buf, int count, int syncoffset)
{
	int i;
	BOOL ret;
	DWORD phase;

#if USE_SYNC_TRANS == 1
	// 同期転送
	if (actmode == TARGET && syncoffset > 0) {
		return SendSyncTransfer(buf, count, syncoffset);
	}
#endif	// USE_SYNC_TRANS == 1

	// IRQ無効
	DisableIRQ();

	if (actmode == TARGET) {
		for (i = 0; i < count; i++) {
			// データ設定
			SetDAT(*buf);

			// ACKネゲート待ち
			ret = WaitSignal(PIN_ACK, OFF);

			// ACKネゲート待ちでタイムアウト
			if (!ret) {
				break;
			}

			// ACKネゲート待ちで既にウェイトが入っている

			// REQアサート
			SetSignal(PIN_REQ, ON);

			// ACKアサート待ち
			ret = WaitSignal(PIN_ACK, ON);

			// REQネゲート
			SetSignal(PIN_REQ, OFF);

			// ACKアサート待ちでタイムアウト
			if (!ret) {
				break;
			}

			// 次データへ
			buf++;
		}

		// ACKネゲート待ち
		WaitSignal(PIN_ACK, OFF);
	} else {
		// フェーズ取得
		phase = Aquire() & GPIO_MCI;

		for (i = 0; i < count; i++) {
			// データ設定
			SetDAT(*buf);

			// REQアサート待ち
			if (!WaitSignal(PIN_REQ, ON)) {
				break;
			}

			// フェーズエラー
			if ((signals & GPIO_MCI) != phase) {
				break;
			}

			// REQアサート待ちで既にウェイトが入っている

			// ACKアサート
			SetSignal(PIN_ACK, ON);

			// REQネゲート待ち
			ret = WaitSignal(PIN_REQ, OFF);

			// ACKネゲート
			SetSignal(PIN_ACK, OFF);

			// REQネゲート待ちでタイムアウト
			if (!ret) {
				break;
			}

			// フェーズエラー
			if ((signals & GPIO_MCI) != phase) {
				break;
			}

			// 次データへ
			buf++;
		}
	}

	// IRQ有効
	EnableIRQ();

	// 送信数を返却
	return i;
}

//---------------------------------------------------------------------------
//
//	一括データ受信ハンドシェイク
//
//---------------------------------------------------------------------------
int FASTCALL GPIOBUS::ReceiveHandShake(BYTE *buf, int count, int syncoffset)
{
	int i;
	BOOL ret;
	DWORD phase;

#if USE_SYNC_TRANS == 1
	// 同期転送
	if (actmode == TARGET && syncoffset > 0) {
		return ReceiveSyncTransfer(buf, count, syncoffset);
	}
#endif	// USE_SYNC_TRANS == 1

	// IRQ無効
	DisableIRQ();

	if (actmode == TARGET) {
		for (i = 0; i < count; i++) {
			// REQアサート
			SetSignal(PIN_REQ, ON);

			// ACKアサート待ち
			ret = WaitSignal(PIN_ACK, ON);

			// 信号線が安定するまでウェイト
			DelayBits();

			// データ取得
			Aquire();
			*buf = GetDAT();

			// REQネゲート
			SetSignal(PIN_REQ, OFF);

			// ACKアサート待ちでタイムアウト
			if (!ret) {
				break;
			}

			// ACKネゲート待ち
			if (!WaitSignal(PIN_ACK, OFF)) {
				break;
			}

			// 次データへ
			buf++;
		}
	} else {
		// フェーズ取得
		phase = Aquire() & GPIO_MCI;

		for (i = 0; i < count; i++) {
			// REQアサート待ち
			if (!WaitSignal(PIN_REQ, ON)) {
				break;
			}

			// フェーズエラー
			if ((signals & GPIO_MCI) != phase) {
				break;
			}

			// 信号線が安定するまでウェイト
			DelayBits();

			// データ取得
			Aquire();
			*buf = GetDAT();

			// ACKアサート
			SetSignal(PIN_ACK, ON);

			// REQネゲート待ち
			ret = WaitSignal(PIN_REQ, OFF);

			// ACKネゲート
			SetSignal(PIN_ACK, OFF);

			// REQネゲート待ちでタイムアウト
			if (!ret) {
				break;
			}

			// フェーズエラー
			if ((signals & GPIO_MCI) != phase) {
				break;
			}

			// 次データへ
			buf++;
		}
	}

	// IRQ有効
	EnableIRQ();

	// 受信数を返却
	return i;
}

#if USE_SYNC_TRANS == 1
//---------------------------------------------------------------------------
//
//	同期データ転送(送信)
//
//---------------------------------------------------------------------------
int FASTCALL GPIOBUS::SendSyncTransfer(
	BYTE *buf, int count, int syncoffset)
{
	BOOL clkev;
	BOOL ackev;
	int req;
	int ack;
	int offset;
	int watchdog;

	ASSERT(actmode == TARGET);
	ASSERT(syncoffset > 0);

	// ACKネゲート待ち
	WaitSignal(PIN_ACK, OFF);

	// IRQ無効
	DisableIRQ();

	// カウンタ初期化
	req = 0;
	ack = 0;
	offset = 0;
	watchdog = 4096;

	// クロックイベント設定
	SetupClockEvent();

	// ACKイベント設定(立下り検出)
	SetupAckEvent(FALSE);

	// 全ての応答が返るまでループ
	while (req < count && ack < count) {
		// イベント取得(タイミングずれ防止のため一度に取得)
		clkev = IsClockEvent();
		ackev = IsAckEvent();

		// ACK受信
		if (ackev) {
			// 応答数加算
			ack++;

			// オフセット減算
			offset--;

			// ウォッチドッグカウンタ更新
			watchdog = 4096;

			// ACKイベントクリア
			ClearAckEvent();
		}

		// REQ送信
		if (clkev || ackev) {
			if (req < count && offset < syncoffset) {
				// リクエスト数加算
				req++;

				// オフセット加算
				offset++;

				// データ設定
				SetDAT(*buf++);

				// 信号線が安定するまでウェイト
				DelayBits();

				// パルス送信
				OutputReqPulse();
			}

			// ウォッチドッグカウンタ確認
			if (--watchdog == 0) {
				break;
			}

			// クロックイベントクリア
			ClearClockEvent();
		}
	}

	// クロックイベントを解除
	ReleaseClockEvent();

	// ACKイベントを解除
	ReleaseAckEvent(FALSE);

	// IRQ有効
	EnableIRQ();

	// 送信数を返却
	return req;
}

//---------------------------------------------------------------------------
//
//	同期データ転送(受信)
//
//---------------------------------------------------------------------------
int FASTCALL GPIOBUS::ReceiveSyncTransfer(
	BYTE *buf, int count, int syncoffset)
{
	int ack;

	ASSERT(actmode == TARGET);
	ASSERT(syncoffset > 0);

	// ACKネゲート待ち
	WaitSignal(PIN_ACK, OFF);

	// IRQ無効
	DisableIRQ();

	// カウンタ初期化
	ack = 0;

	// ACKイベント設定(立下り検出)
	SetupAckEvent(FALSE);

	// 全ての応答が返るまでループ
	while (ack < count) {
		// ACKイベントクリア
		ClearAckEvent();

		// REQパルス送信
		OutputReqPulse();

		// ACK受信待ち
		if (!WaitAckEvent()) {
			break;
		}

		// ACK受信
		ack++;

		// データを取得
		Aquire();
		*buf++ = GetDAT();
	}

	// ACKイベントを解除
	ReleaseAckEvent(FALSE);

	// IRQ有効
	EnableIRQ();

	// 受信数を返却
	return ack;
}

//---------------------------------------------------------------------------
//
//	ACKイベント初期化
//
//---------------------------------------------------------------------------
void FASTCALL GPIOBUS::SetupAckEvent(BOOL rise)
{
	// 立上がりと立下りで選択する
#if SIGNAL_CONTROL_MODE < 2
	if (rise) {
		gpio[GPIO_AFEN_0 + (PIN_ACK / 32)] |= SIGBIT(PIN_ACK);
	} else {
		gpio[GPIO_AREN_0 + (PIN_ACK / 32)] |= SIGBIT(PIN_ACK);
	}
#else
	if (rise) {
		gpio[GPIO_AREN_0 + (PIN_ACK / 32)] |= SIGBIT(PIN_ACK);
	} else {
		gpio[GPIO_AFEN_0 + (PIN_ACK / 32)] |= SIGBIT(PIN_ACK);
	}
#endif	// SIGNAL_CONTROL_MODE

	// クリア
	gpio[GPIO_EDS_0 + (PIN_ACK / 32)] = SIGBIT(PIN_ACK);
}

//---------------------------------------------------------------------------
//
//	ACKイベント解放
//
//---------------------------------------------------------------------------
void FASTCALL GPIOBUS::ReleaseAckEvent(BOOL rise)
{
	// 立上がりと立下りで選択する
#if SIGNAL_CONTROL_MODE < 2
	if (rise) {
		gpio[GPIO_AFEN_0 + (PIN_ACK / 32)] &= ~SIGBIT(PIN_ACK);
	} else {
		gpio[GPIO_AREN_0 + (PIN_ACK / 32)] &= ~SIGBIT(PIN_ACK);
	}
#else
	if (rise) {
		gpio[GPIO_AREN_0 + (PIN_ACK / 32)] &= ~SIGBIT(PIN_ACK);
	} else {
		gpio[GPIO_AFEN_0 + (PIN_ACK / 32)] &= ~SIGBIT(PIN_ACK);
	}
#endif	// SIGNAL_CONTROL_MODE

	// クリア
	gpio[GPIO_EDS_0 + (PIN_ACK / 32)] = SIGBIT(PIN_ACK);
}

//---------------------------------------------------------------------------
//
//	ACKイベント検証
//
//---------------------------------------------------------------------------
BOOL FASTCALL GPIOBUS::IsAckEvent()
{
	return gpio[GPIO_EDS_0 + (PIN_ACK / 32)] & SIGBIT(PIN_ACK);
}

//---------------------------------------------------------------------------
//
//	ACKイベントクリア
//
//---------------------------------------------------------------------------
void FASTCALL GPIOBUS::ClearAckEvent()
{
	gpio[GPIO_EDS_0 + (PIN_ACK / 32)] = SIGBIT(PIN_ACK);
}

//---------------------------------------------------------------------------
//
//	ACKイベント待ち
//
//---------------------------------------------------------------------------
BOOL FASTCALL GPIOBUS::WaitAckEvent()
{
	int watchdog;

	// ウォッチドッグカウンタ初期化
	watchdog = GPIO_WATCHDOG_MAX;

	// イベント待ち
	while (!IsAckEvent()) {
		// ウォッチドッグカウンタ減算
		watchdog--;

		// タイムアウト
		if (watchdog == 0) {
			return FALSE;
		}
	}

	return TRUE;
}
#endif	// USE_SYNC_TRANS == 1

//---------------------------------------------------------------------------
//
//	微小時間ウェイト
//
//---------------------------------------------------------------------------
void FASTCALL GPIOBUS::DelayBits()
{
	SysTimer::SleepNsec(GPIO_DATA_SETTLING);
}

#if USE_SYNC_TRANS == 1
//---------------------------------------------------------------------------
//
//	クロックイベント初期化
//
//---------------------------------------------------------------------------
void FASTCALL GPIOBUS::SetupClockEvent()
{
	gpio[GPIO_AFEN_0 + (PIN_GPCLK / 32)] |= SIGBIT(PIN_GPCLK);
	gpio[GPIO_EDS_0 + (PIN_GPCLK / 32)] = SIGBIT(PIN_GPCLK);
}

//---------------------------------------------------------------------------
//
//	クロックイベント解放
//
//---------------------------------------------------------------------------
void FASTCALL GPIOBUS::ReleaseClockEvent()
{
	gpio[GPIO_AFEN_0 + (PIN_GPCLK / 32)] &= ~SIGBIT(PIN_GPCLK);
	gpio[GPIO_EDS_0 + (PIN_GPCLK / 32)] = SIGBIT(PIN_GPCLK);
}

//---------------------------------------------------------------------------
//
//	クロックイベント検証
//
//---------------------------------------------------------------------------
BOOL FASTCALL GPIOBUS::IsClockEvent()
{
	return gpio[GPIO_EDS_0 + (PIN_GPCLK / 32)] & SIGBIT(PIN_GPCLK);
}

//---------------------------------------------------------------------------
//
//	クロックイベントクリア
//
//---------------------------------------------------------------------------
void FASTCALL GPIOBUS::ClearClockEvent()
{
	gpio[GPIO_EDS_0 + (PIN_GPCLK / 32)] = SIGBIT(PIN_GPCLK);
}

//---------------------------------------------------------------------------
//
//	REQパルス出力
//
//---------------------------------------------------------------------------
void FASTCALL GPIOBUS::OutputReqPulse()
{
#if SIGNAL_CONTROL_MODE == 0
	int index;
	int shift;
	DWORD sel;
	DWORD ast;

	index = PIN_REQ / 10;
	shift = (PIN_REQ % 10) * 3;
	sel = gpfsel[index];
	ast = sel | (1 << shift);
	gpio[index] = ast;
	MemoryBarrier();
	gpio[index] = sel;
	MemoryBarrier();
#elif SIGNAL_CONTROL_MODE == 1
	gpio[GPIO_CLR_0] = 0x1 << PIN_REQ;
	MemoryBarrier();
	gpio[GPIO_SET_0] = 0x1 << PIN_REQ;
	MemoryBarrier();
#elif SIGNAL_CONTROL_MODE == 2
	gpio[GPIO_SET_0] = 0x1 << PIN_REQ;
	MemoryBarrier();
	gpio[GPIO_CLR_0] = 0x1 << PIN_REQ;
	MemoryBarrier();
#endif	// SIGNAL_CONTROL_MODE
}
#endif	// USE_SYNC_TRANS == 1

//---------------------------------------------------------------------------
//
//	SEL信号イベントポーリング
//
//---------------------------------------------------------------------------
int FASTCALL GPIOBUS::PollSelectEvent()
{
	// errnoクリア
	errno = 0;

#ifdef BAREMETAL
	int ret;

	// 初期化
	ret = 0;

	// 割り込み有効
	EnableInterrupts();

	// 割り込み待ち
	WaitForInterrupts();

	// 割り込み無効
	DisableInterrupts();

	// イベント返却
	Aquire();
	if (GetSEL()) {
		ret |= 1;
	}

	if (GetRST()) {
		ret |= 2;
	}

	return ret;
#else
	struct epoll_event epev[2];
	struct gpioevent_data gpev;
	int nfds;
	int i;
	int ret;

	// 初期化
	ret = 0;

	// 割り込み待ち
	nfds = epoll_wait(epfd, epev, 2, -1);
	if (nfds <= 0) {
		return -1;
	}

	// イベント返却
	for (i = 0; i < nfds; i++) {
		if (epev[i].data.fd == selevreq.fd) {
			read(selevreq.fd, &gpev, sizeof(gpev));
			ret |= 1;
		} else if (epev[i].data.fd == rstevreq.fd) {
			read(rstevreq.fd, &gpev, sizeof(gpev));
			ret |= 2;
		}
	}
#endif	// BAREMETAL

	return ret;
}

//---------------------------------------------------------------------------
//
//	SEL信号イベント解除
//
//---------------------------------------------------------------------------
void FASTCALL GPIOBUS::ClearSelectEvent()
{
#ifdef BAREMETAL
	DWORD irq;

	// イベントクリア
	gpio[GPIO_EDS_0] = (1 << PIN_SEL) | (1 << PIN_RST);

	// GICへの応答
	if (rpitype == 4) {
		// IRQ番号
		irq = gicc[GICC_IAR] & 0x3FF;

		// 割り込み応答
		gicc[GICC_EOIR] = irq;
	}
#endif	// BAREMETAL
}

//---------------------------------------------------------------------------
//
//	信号テーブル
//
//---------------------------------------------------------------------------
const int GPIOBUS::SignalTable[19] = {
	PIN_DT0, PIN_DT1, PIN_DT2, PIN_DT3,
	PIN_DT4, PIN_DT5, PIN_DT6, PIN_DT7,	PIN_DP,
	PIN_SEL,PIN_ATN, PIN_RST, PIN_ACK,
	PIN_BSY, PIN_MSG, PIN_CD, PIN_IO, PIN_REQ,
	-1
};

//---------------------------------------------------------------------------
//
//	ワークテーブル作成
//
//---------------------------------------------------------------------------
void FASTCALL GPIOBUS::MakeTable(void)
{
	const int pintbl[] = {
		PIN_DT0, PIN_DT1, PIN_DT2, PIN_DT3, PIN_DT4,
		PIN_DT5, PIN_DT6, PIN_DT7, PIN_DP
	};

	int i;
	int j;
	BOOL tblParity[256];
	DWORD bits;
	DWORD parity;
#if SIGNAL_CONTROL_MODE == 0
	int index;
	int shift;
#else
	DWORD gpclr;
	DWORD gpset;
#endif

	// パリティテーブル作成
	for (i = 0; i < 0x100; i++) {
		bits = (DWORD)i;
		parity = 0;
		for (j = 0; j < 8; j++) {
			parity ^= bits & 1;
			bits >>= 1;
		}
		parity = ~parity;
		tblParity[i] = parity & 1;
	}

#if SIGNAL_CONTROL_MODE == 0
	// マスクと設定データ生成
	memset(tblDatMsk, 0xff, sizeof(tblDatMsk));
	memset(tblDatSet, 0x00, sizeof(tblDatSet));
	for (i = 0; i < 0x100; i++) {
		// 検査用ビット列
		bits = (DWORD)i;

		// パリティ取得
		if (tblParity[i]) {
			bits |= (1 << 8);
		}

		// ビット検査
		for (j = 0; j < 9; j++) {
			// インデックスとシフト量計算
			index = pintbl[j] / 10;
			shift = (pintbl[j] % 10) * 3;

			// マスクデータ
			tblDatMsk[index][i] &= ~(0x7 << shift);

			// 設定データ
			if (bits & 1) {
				tblDatSet[index][i] |= (1 << shift);
			}

			bits >>= 1;
		}
	}
#else
	// マスクと設定データ生成
	memset(tblDatMsk, 0x00, sizeof(tblDatMsk));
	memset(tblDatSet, 0x00, sizeof(tblDatSet));
	for (i = 0; i < 0x100; i++) {
		// 検査用ビット列
		bits = (DWORD)i;

		// パリティ取得
		if (tblParity[i]) {
			bits |= (1 << 8);
		}

#if SIGNAL_CONTROL_MODE == 1
		// 負論理は反転
		bits = ~bits;
#endif

		// GPIOレジスタ情報の作成
		gpclr = 0;
		gpset = 0;
		for (j = 0; j < 9; j++) {
			if (bits & 1) {
				gpset |= (1 << pintbl[j]);
			} else {
				gpclr |= (1 << pintbl[j]);
			}
			bits >>= 1;
		}

		tblDatMsk[i] = gpclr;
		tblDatSet[i] = gpset;
	}
#endif
}

//---------------------------------------------------------------------------
//
//	制御信号設定
//
//---------------------------------------------------------------------------
void FASTCALL GPIOBUS::SetControl(int pin, BOOL ast)
{
	PinSetSignal(pin, ast);

	// メモリバリア
	MemoryBarrier();
}

//---------------------------------------------------------------------------
//
//	入出力モード設定
//
//---------------------------------------------------------------------------
void FASTCALL GPIOBUS::SetMode(int pin, int mode)
{
	int index;
	int shift;
	DWORD data;

#if SIGNAL_CONTROL_MODE == 0
	if (mode == OUT) {
		return;
	}
#endif	// SIGNAL_CONTROL_MODE

	index = pin / 10;
	shift = (pin % 10) * 3;
	data = gpfsel[index];
	data &= ~(0x7 << shift);
	if (mode == OUT) {
		data |= (1 << shift);
	}
	gpio[index] = data;
	gpfsel[index] = data;

	// メモリバリア
	MemoryBarrier();
}
	
//---------------------------------------------------------------------------
//
//	入力信号値取得
//
//---------------------------------------------------------------------------
BOOL FASTCALL GPIOBUS::GetSignal(int pin) const
{
	return  (signals >> pin) & 1;
}
	
//---------------------------------------------------------------------------
//
//	出力信号値設定
//
//---------------------------------------------------------------------------
void FASTCALL GPIOBUS::SetSignal(int pin, BOOL ast)
{
#if SIGNAL_CONTROL_MODE == 0
	int index;
	int shift;
	DWORD data;

	index = pin / 10;
	shift = (pin % 10) * 3;
	data = gpfsel[index];
	if (ast) {
		data |= (1 << shift);
	} else {
		data &= ~(0x7 << shift);
	}
	gpio[index] = data;
	gpfsel[index] = data;
#elif SIGNAL_CONTROL_MODE == 1
	if (ast) {
		gpio[GPIO_CLR_0] = 0x1 << pin;
	} else {
		gpio[GPIO_SET_0] = 0x1 << pin;
	}
#elif SIGNAL_CONTROL_MODE == 2
	if (ast) {
		gpio[GPIO_SET_0] = 0x1 << pin;
	} else {
		gpio[GPIO_CLR_0] = 0x1 << pin;
	}
#endif	// SIGNAL_CONTROL_MODE

	// メモリバリア
	MemoryBarrier();
}

//---------------------------------------------------------------------------
//
//	信号変化待ち
//
//---------------------------------------------------------------------------
BOOL FASTCALL GPIOBUS::WaitSignal(int pin, BOOL ast)
{
	DWORD sig;
	DWORD now;
	DWORD timeout;

	// 現在
	now = SysTimer::GetTimerLow();

	// タイムアウト時間
	timeout = GPIO_TIMEOUT_MAX;

	// 変化したら即終了
	do {
		// リセットを受信したら即終了
#if SIGNAL_CONTROL_MODE < 2
		sig = ~*level;
#else
		sig = *level;
#endif	// SIGNAL_CONTROL_MODE
		if (sig & SIGBIT(PIN_RST)) {
			signals = sig;
			return FALSE;
		}

		// エッジを検出したら
		if (((sig >> pin) ^ ~ast) & 1) {
			signals = sig;
			return TRUE;
		}
	} while ((SysTimer::GetTimerLow() - now) < timeout);

	// タイムアウト
	signals = sig;
	return FALSE;
}

//---------------------------------------------------------------------------
//
//	IRQ禁止
//
//---------------------------------------------------------------------------
void FASTCALL GPIOBUS::DisableIRQ()
{
#ifndef BAREMETAL
	if (rpitype == 4) {
		// RPI4はGICCで割り込み禁止に設定
		giccpmr = gicc[GICC_PMR];
		gicc[GICC_PMR] = 0;
	} else if (rpitype == 2) {
		// RPI2,3はコアタイマーIRQを無効にする
		tintcore = sched_getcpu() + QA7_CORE0_TINTC;
		tintctl = qa7regs[tintcore];
		qa7regs[tintcore] = 0;
	} else {
		// 割り込みコントローラでシステムタイマー割り込みを止める
		irptenb = irpctl[IRPT_ENB_IRQ_1];
		irpctl[IRPT_DIS_IRQ_1] = irptenb & 0xf;
	}

	// メモリバリア
	MemoryBarrier();
#endif	// BAREMETAL
}

//---------------------------------------------------------------------------
//
//	IRQ有効
//
//---------------------------------------------------------------------------
void FASTCALL GPIOBUS::EnableIRQ()
{
#ifndef BAREMETAL
	if (rpitype == 4) {
		// RPI4はGICCを割り込み許可に設定
		gicc[GICC_PMR] = giccpmr;
	} else if (rpitype == 2) {
		// RPI2,3はコアタイマーIRQを有効に戻す
		qa7regs[tintcore] = tintctl;
	} else {
		// 割り込みコントローラでシステムタイマー割り込みを再開
		irpctl[IRPT_ENB_IRQ_1] = irptenb & 0xf;
	}

	// メモリバリア
	MemoryBarrier();
#endif	// BAREMETAL
}

//---------------------------------------------------------------------------
//
//	ピン機能設定(入出力設定)
//
//---------------------------------------------------------------------------
void FASTCALL GPIOBUS::PinConfig(int pin, int mode)
{
	int index;
	DWORD mask;

	// 未使用なら無効
	if (pin < 0) {
		return;
	}

	index = GPIO_FSEL_0 + pin / 10;
	mask = ~(0x7 << ((pin % 10) * 3));
	gpio[index] = (gpio[index] & mask) | ((mode & 0x7) << ((pin % 10) * 3));
}

//---------------------------------------------------------------------------
//
//	ピン機能設定(プルアップ/ダウン)
//
//---------------------------------------------------------------------------
void FASTCALL GPIOBUS::PullConfig(int pin, int mode)
{
	int shift;
	DWORD bits;
	DWORD pull;

	// 未使用なら無効
	if (pin < 0) {
		return;
	}

	if (rpitype == 4) {
		switch (mode) {
			case GPIO_PULLNONE:
				pull = 0;
				break;
			case GPIO_PULLUP:
				pull = 2;
				break;
			case GPIO_PULLDOWN:
				pull = 1;
				break;
			default:
				return;
		}

		pin &= 0x1f;
		shift = (pin & 0xf) << 1;
		bits = gpio[GPIO_PUPPDN0 + (pin >> 4)];
		bits &= ~(3 << shift);
		bits |= (pull << shift);
		gpio[GPIO_PUPPDN0 + (pin >> 4)] = bits;
	} else {
		pin &= 0x1f;
		gpio[GPIO_PUD] = mode & 0x3;
		SysTimer::SleepUsec(2);
		gpio[GPIO_CLK_0] = 0x1 << pin;
		SysTimer::SleepUsec(2);
		gpio[GPIO_PUD] = 0;
		gpio[GPIO_CLK_0] = 0;
	}
}

//---------------------------------------------------------------------------
//
//	ピン出力設定
//
//---------------------------------------------------------------------------
void FASTCALL GPIOBUS::PinSetSignal(int pin, BOOL ast)
{
	// 未使用なら無効
	if (pin < 0) {
		return;
	}

	if (ast) {
		gpio[GPIO_SET_0] = 0x1 << pin;
	} else {
		gpio[GPIO_CLR_0] = 0x1 << pin;
	}
}

//---------------------------------------------------------------------------
//
//	Drive Strength設定
//
//---------------------------------------------------------------------------
void FASTCALL GPIOBUS::DrvConfig(DWORD drive)
{
	DWORD data;

	data = pads[PAD_0_27];
	pads[PAD_0_27] = (0xFFFFFFF8 & data) | drive | 0x5a000000;
}

//---------------------------------------------------------------------------
//
//	システムタイマアドレス
//
//---------------------------------------------------------------------------
volatile DWORD* SysTimer::systaddr;

//---------------------------------------------------------------------------
//
//	ARMタイマアドレス
//
//---------------------------------------------------------------------------
volatile DWORD* SysTimer::armtaddr;

//---------------------------------------------------------------------------
//
//	コア周波数
//
//---------------------------------------------------------------------------
volatile DWORD SysTimer::corefreq;

//---------------------------------------------------------------------------
//
//	システムタイマ初期化
//
//---------------------------------------------------------------------------
void FASTCALL SysTimer::Init(DWORD *syst, DWORD *armt)
{
#ifndef BAREMETAL
	// RPI Mailbox property interface
	// Get max clock rate
	//  Tag: 0x00030004
	//
	//  Request: Length: 4
	//   Value: u32: clock id
	//  Response: Length: 8
	//   Value: u32: clock id, u32: rate (in Hz)
	//
	// Clock id
	//  0x000000004: CORE
	DWORD maxclock[32] = { 32, 0, 0x00030004, 8, 0, 4, 0, 0 };
	int fd;
#endif	// BAREMETAL

	// ベースアドレス保存
	systaddr = syst;
	armtaddr = armt;

	// ARMタイマをフリーランモードに変更
	armtaddr[ARMT_CTRL] = 0x00000282;

	// コア周波数取得
#ifdef BAREMETAL
	corefreq = ARM_getcorespeed() / 1000000;
#else
	corefreq = 0;
	fd = open("/dev/vcio", O_RDONLY);
	if (fd >= 0) {
		ioctl(fd, _IOWR(100, 0, char *), maxclock);
		corefreq = maxclock[6] / 1000000;
	}
	close(fd);
#endif	// BAREMETAL
}

//---------------------------------------------------------------------------
//
//	システムタイマ取得
//
//---------------------------------------------------------------------------
UL64 FASTCALL SysTimer::GetTimer()
{
	DWORD hi;
	DWORD lo;

	do {
		hi = systaddr[SYST_CHI];
		lo = systaddr[SYST_CLO];
	} while (hi != systaddr[SYST_CHI]);

	return (((UL64)hi << 32) | lo);
}

//---------------------------------------------------------------------------
//
//	システムタイマ(LO)取得
//
//---------------------------------------------------------------------------
DWORD FASTCALL SysTimer::GetTimerLow() {
	return systaddr[SYST_CLO];
}

//---------------------------------------------------------------------------
//
//	システムタイマ(HI)取得
//
//---------------------------------------------------------------------------
DWORD FASTCALL SysTimer::GetTimerHigh() {
	return systaddr[SYST_CHI];
}

//---------------------------------------------------------------------------
//
//	ナノ秒単位のスリープ
//
//---------------------------------------------------------------------------
void FASTCALL SysTimer::SleepNsec(DWORD nsec)
{
	DWORD diff;
	DWORD start;

	// ウェイトしない
	if (nsec == 0) {
		return;
	}

	// タイマー差を算出
	diff = corefreq * nsec / 1000;

	// 微小なら復帰
	if (diff == 0) {
		return;
	}

	// 開始
	start = armtaddr[ARMT_FREERUN];

	// カウントオーバーまでループ
	while ((armtaddr[ARMT_FREERUN] - start) < diff);
}

//---------------------------------------------------------------------------
//
//	μ秒単位のスリープ
//
//---------------------------------------------------------------------------
void FASTCALL SysTimer::SleepUsec(DWORD usec)
{
	DWORD now;

	// ウェイトしない
	if (usec == 0) {
		return;
	}

	now = GetTimerLow();
	while ((GetTimerLow() - now) < usec);
}

extern "C"
//---------------------------------------------------------------------------
//
//	タイマー取得(外部公開用)
//
//---------------------------------------------------------------------------
DWORD GetTimeUs()
{
	return SysTimer::GetTimerLow();
}

extern "C"
//---------------------------------------------------------------------------
//
//	タイマーウェイト(外部公開用)
//
//---------------------------------------------------------------------------
void SleepUs(int us)
{
	SysTimer::SleepUsec(us);
}