//---------------------------------------------------------------------------
//
//	SCSI Target Emulator RaSCSI (*^..^*)
//	for Raspberry Pi
//	Powered by XM6 TypeG Technology.
//
//	Copyright (C) 2016-2021 GIMONS(Twitter:@kugimoto0715)
//
//	[ メイン ]
//
//---------------------------------------------------------------------------

#include "os.h"
#include "rascsi.h"
#include "filepath.h"
#include "fileio.h"
#include "disk.h"
#if USE_BRIDGE_NET == 1
#include "netdriver.h"
#endif	// USE_BRIDGE_NET == 1
#if USE_BRIDGE_FS == 1
#include "fsdriver.h"
#endif	// USE_BRIDGE_FS == 1
#include "gpiobus.h"

//---------------------------------------------------------------------------
//
//	定数宣言
//
//---------------------------------------------------------------------------
#define CtrlMax	8					// 最大SCSIコントローラ数
#define UnitNum	2					// コントローラ毎ののユニット数

//---------------------------------------------------------------------------
//
//	列挙子定義
//
//---------------------------------------------------------------------------
enum execprio_e {
	PRIO_NORMAL		= 0,		// ノーマル
	PRIO_MIN		= 1,		// 最低
	PRIO_MAX		= 2			// 最高
};

//---------------------------------------------------------------------------
//
//	変数宣言
//
//---------------------------------------------------------------------------
static volatile BOOL running;		// 実行中フラグ
static volatile BOOL active;		// 処理中フラグ
char logbuf[4096];					// ログバッファ
SASIDEV *ctrl[CtrlMax];				// コントローラ
Disk *disk[CtrlMax * UnitNum];		// ディスク
GPIOBUS *bus;						// GPIOバス
SCSIBR *scsibr;						// ブリッジデバイス
#ifdef BAREMETAL
FATFS fatfs;						// FatFS
#else
int monsocket;						// モニター用ソケット
pthread_t monthread;				// モニタースレッド
static void *MonThread(void *param);
#endif	// BAREMETAL
BOOL haltreq;						// HALT要求

#if USE_BRIDGE_NET == 1
NetDriver *netdrv;					// ネットワークドライバ
#endif	// USE_BRIDGE_NET == 1

#if USE_BRIDGE_FS == 1
FsDriver *fsdrv;					// ファイルシステムドライバ
#endif	// USE_BRIDGE_FS == 1

//---------------------------------------------------------------------------
//
//	プロトタイプ宣言
//
//---------------------------------------------------------------------------
int CtlCallback(BOOL, int, int, int, BYTE *);
int NetCallback(BOOL read, int func, int phase, int len, BYTE *buf);
int FsCallback(BOOL read, int func, int phase, int len, BYTE *buf);

//---------------------------------------------------------------------------
//
//	スレッドを特定のCPUに固定する
//
//---------------------------------------------------------------------------
void FixCpu(int cpu)
{
#ifndef BAREMETAL
	cpu_set_t cpuset;
	int cpus;

	// CPU数取得
	CPU_ZERO(&cpuset);
	sched_getaffinity(0, sizeof(cpu_set_t), &cpuset);
	cpus = CPU_COUNT(&cpuset);

	// アフィニティ設定
	if (cpu < cpus) {
		CPU_ZERO(&cpuset);
		CPU_SET(cpu, &cpuset);
		sched_setaffinity(0, sizeof(cpu_set_t), &cpuset);
	}
#endif	// !BAREMETAL
}

//---------------------------------------------------------------------------
//
//	実行優先順位設定
//
//---------------------------------------------------------------------------
void SetExecPrio(execprio_e prio)
{
#ifndef BAREMETAL
	pthread_t pt;
	struct sched_param schedparam;

	pt = pthread_self();

	switch (prio) {
		case PRIO_NORMAL:
			schedparam.sched_priority = 0;
			pthread_setschedparam(pt, SCHED_OTHER, &schedparam);
			break;

		case PRIO_MIN:
			schedparam.sched_priority = 0;
			pthread_setschedparam(pt, SCHED_IDLE, &schedparam);
			break;

		case PRIO_MAX:
			schedparam.sched_priority = sched_get_priority_min(SCHED_FIFO);
			pthread_setschedparam(pt, SCHED_FIFO, &schedparam);
			break;
	}
#endif	// !BAREMETAL
}

#ifndef BAREMETAL
//---------------------------------------------------------------------------
//
//	シグナル処理
//
//---------------------------------------------------------------------------
void KillHandler(int sig)
{
	// 停止指示
	running = FALSE;
}
#endif	// BAREMETAL

//---------------------------------------------------------------------------
//
//	ログ出力
//
//---------------------------------------------------------------------------
void LogWrite(FILE *fp, const char *format, ...)
{
	char buffer[0x200];
	va_list args;
	va_start(args, format);

	// フォーマット
	vsprintf(buffer, format, args);

	// 可変長引数終了
	va_end(args);

	// ログ出力
	if (!fp) {
		strcat(logbuf, buffer);
	} else {
#ifdef BAREMETAL
		printf(buffer);
#else
		fprintf(fp, buffer);
#endif	// BAREMETAL
	}
}

//---------------------------------------------------------------------------
//
//	バナー出力
//
//---------------------------------------------------------------------------
void Banner(int argc, char* argv[])
{
	LogWrite(stdout,"SCSI Target Emulator RaSCSI(*^..^*) ");
	LogWrite(stdout,"version %01d.%01d%01d(%s, %s)\n",
		(int)((VERSION >> 8) & 0xf),
		(int)((VERSION >> 4) & 0xf),
		(int)((VERSION     ) & 0xf),
		__DATE__,
		__TIME__);
	LogWrite(stdout,"Powered by XM6 TypeG Technology / ");
	LogWrite(stdout,"Copyright (C) 2016-2021 GIMONS\n");
#if USE_BURST_BUS == 1 && USE_SYNC_TRANS == 1
	LogWrite(stdout,"Synchronous Transfer support\n");
#endif	// USE_BURST_BUS == 1 && USE_SYNC_TRANS == 1
	LogWrite(stdout,"Connect type : %s\n", CONNECT_DESC);

	if (argc > 1 && strcmp(argv[1], "-h") == 0) {
		LogWrite(stdout,"\n");
		LogWrite(stdout,"Usage: %s [-IDn FILE] ...\n\n", argv[0]);
		LogWrite(stdout," n is SCSI identification number(0-7).\n");
		LogWrite(stdout," FILE is disk image file.\n\n");
		LogWrite(stdout,"Usage: %s [-HDn FILE] ...\n\n", argv[0]);
		LogWrite(stdout," n is X68000 SASI HD number(0-15).\n");
		LogWrite(stdout," FILE is disk image file.\n\n");
		LogWrite(stdout," Image type is detected based on file extension.\n");
		LogWrite(stdout,"  hdf : SASI HD image(XM6 SASI HD image)\n");
		LogWrite(stdout,"  hds : SCSI HD image(XM6 SCSI HD image)\n");
		LogWrite(stdout,"  hdn : SCSI HD image(NEC GENUINE)\n");
		LogWrite(stdout,"  hdi : SCSI HD image(Anex86 HD image)\n");
		LogWrite(stdout,"  nhd : SCSI HD image(T98Next HD image)\n");
		LogWrite(stdout,"  hda : SCSI HD image(APPLE GENUINE)\n");
		LogWrite(stdout,"  mos : SCSI MO image(XM6 SCSI MO image)\n");
		LogWrite(stdout,"  iso : SCSI CD image(ISO 9660 image)\n\n");
		LogWrite(stdout,"Usage: %s CONFIG_FILE\n\n", argv[0]);
		LogWrite(stdout," CONFIG_FILE is disk images config file.\n");

#ifndef BAREMETAL
		exit(0);
#endif	// BAREMETAL
	}
}

//---------------------------------------------------------------------------
//
//	初期化
//
//---------------------------------------------------------------------------
BOOL Init()
{
	int i;
#ifdef BAREMETAL
	FRESULT fr;
#endif	// BAREMETAL

#ifdef BAREMETAL
	// SDカードマウント
	fr = f_mount(&fatfs, "", 1);
	if (fr != FR_OK) {
		LogWrite(stderr, "Error : SD card mount failed.\n");
		return FALSE;
	}
#endif	// BAREMETAL

#ifndef BAREMETAL
	struct sockaddr_in server;
	int yes;

	// モニター用ソケット生成
	monsocket = socket(PF_INET, SOCK_STREAM, 0);
	memset(&server, 0, sizeof(server));
	server.sin_family = PF_INET;
	server.sin_port   = htons(6868);
	server.sin_addr.s_addr = htonl(INADDR_ANY);

	// アドレスの再利用を許可する
	yes = 1;
	if (setsockopt(
		monsocket, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes)) < 0){
		return FALSE;
	}

	// バインド
	if (bind(monsocket, (struct sockaddr *)&server,
		sizeof(struct sockaddr_in)) < 0) {
		LogWrite(stderr, "Error : Already running?\n");
		return FALSE;
	}

	// モニタースレッド生成
	pthread_create(&monthread, NULL, MonThread, NULL);

	// 割り込みハンドラ設定
	if (signal(SIGINT, KillHandler) == SIG_ERR) {
		return FALSE;
	}
	if (signal(SIGHUP, KillHandler) == SIG_ERR) {
		return FALSE;
	}
	if (signal(SIGTERM, KillHandler) == SIG_ERR) {
		return FALSE;
	}
	if (signal(SIGALRM, KillHandler) == SIG_ERR) {
		return FALSE;
	}
#endif // BAREMETAL

	// GPIOBUS生成
	bus = new GPIOBUS();
	
	// GPIO初期化
	if (!bus->Init()) {
		return FALSE;
	}

	// ターゲットモード
	bus->SetMode(GPIOBUS::TARGET);

	// バスリセット
	bus->Reset();

	// コントローラ初期化
	for (i = 0; i < CtrlMax; i++) {
		ctrl[i] = NULL;
	}

	// ディスク初期化
	for (i = 0; i < CtrlMax; i++) {
		disk[i] = NULL;
	}

	// ホストブリッジ
	scsibr = new SCSIBR();
	scsibr->SetMsgFunc(0, CtlCallback);

#if USE_BRIDGE_NET == 1
	// ネットワークドライバ
	netdrv = new NetDriver();
	if (netdrv->IsNetEnable()) {
		scsibr->SetMsgFunc(1, NetCallback);
	}
#endif	// USE_BRIDGE_NET == 1

#if USE_BRIDGE_FS == 1
	// ファイルシステムドライバ
	fsdrv = new FsDriver();
	scsibr->SetMsgFunc(2, FsCallback);
#endif	// USE_BRIDGE_FS == 1

	// その他
	running = FALSE;
	active = FALSE;
	haltreq = FALSE;

	return TRUE;
}

//---------------------------------------------------------------------------
//
//	クリーンアップ
//
//---------------------------------------------------------------------------
void Cleanup()
{
	int i;

#if USE_BRIDGE_NET == 1
	// ネットワークドライバ削除
	if (netdrv) {
		delete netdrv;
	}
#endif	// USE_BRIDGE_NET == 1

#if USE_BRIDGE_FS == 1
	// ファイルシステムドライバ削除
	if (fsdrv) {
		delete fsdrv;
	}
#endif	// USE_BRIDGE_FS == 1

	// ブリッジデバイス削除
	if (scsibr) {
		delete scsibr;
	}

	// ディスク削除
	for (i = 0; i < CtrlMax * UnitNum; i++) {
		if (disk[i]) {
			if (disk[i] != scsibr) {
				delete disk[i];
			}
			disk[i] = NULL;
		}
	}

	// コントローラ削除
	for (i = 0; i < CtrlMax; i++) {
		if (ctrl[i]) {
			delete ctrl[i];
			ctrl[i] = NULL;
		}
	}

	// バスをクリーンアップ
	bus->Cleanup();
	
	// GPIOBUS破棄
	delete bus;

#ifndef BAREMETAL
	// モニター用ソケットクローズ
	if (monsocket >= 0) {
		close(monsocket);
	}
#endif // BAREMETAL
}

//---------------------------------------------------------------------------
//
//	リセット
//
//---------------------------------------------------------------------------
void Reset()
{
	int i;

	// コントローラリセット
	for (i = 0; i < CtrlMax; i++) {
		if (ctrl[i]) {
			ctrl[i]->Reset();
		}
	}

	// バス信号線をリセット
	bus->Reset();
}

//---------------------------------------------------------------------------
//
//	デバイス一覧表示
//
//---------------------------------------------------------------------------
void ListDevice(FILE *fp)
{
	int i;
	int id;
	int un;
	Disk *pUnit;
	Filepath filepath;
	BOOL find;
	char type[5];

	find = FALSE;
	type[4] = 0;
	for (i = 0; i < CtrlMax * UnitNum; i++) {
		// IDとユニット
		id = i / UnitNum;
		un = i % UnitNum;
		pUnit = disk[i];

		// ユニットが存在しないまたはヌルディスクならスキップ
		if (pUnit == NULL || pUnit->IsNULL()) {
			continue;
		}

		// ヘッダー出力
		if (!find) {
			LogWrite(fp, "+----+----+------+-------------------------------------\n");
			LogWrite(fp, "| ID | UN | TYPE | DEVICE STATUS\n");
			LogWrite(fp, "+----+----+------+-------------------------------------\n");
			find = TRUE;
		}

		// ID,UNIT,タイプ出力
		type[0] = (char)(pUnit->GetID() >> 24);
		type[1] = (char)(pUnit->GetID() >> 16);
		type[2] = (char)(pUnit->GetID() >> 8);
		type[3] = (char)(pUnit->GetID());
		LogWrite(fp, "|  %d |  %d | %s | ", id, un, type);

		// マウント状態出力
		if (pUnit->GetID() == MAKEID('S', 'C', 'B', 'R')) {
			LogWrite(fp, "%s", "HOST BRIDGE");
		} else {
			pUnit->GetPath(filepath);
			LogWrite(fp, "%s",
				(pUnit->IsRemovable() && !pUnit->IsReady()) ?
				"NO MEDIA" : filepath.GetPath());
		}

		// ライトプロテクト状態出力
		if (pUnit->IsRemovable() && pUnit->IsReady() && pUnit->IsWriteP()) {
			LogWrite(fp, "(WRITEPROTECT)");
		}

		// 次の行へ
		LogWrite(fp, "\n");
	}

	// コントローラが無い場合
	if (!find) {
		LogWrite(fp, "No device is installed.\n");
		return;
	}
	
	LogWrite(fp, "+----+----+------+-------------------------------------\n");
}

//---------------------------------------------------------------------------
//
//	コントローラマッピング
//
//---------------------------------------------------------------------------
void MapControler(FILE *fp, Disk **map)
{
	int i;
	int j;
	int unitno;
	int sasi_num;
	int scsi_num;

	// 変更されたユニットの置き換え
	for (i = 0; i < CtrlMax; i++) {
		for (j = 0; j < UnitNum; j++) {
			unitno = i * UnitNum + j;
			if (disk[unitno] != map[unitno]) {
				// 元のユニットが存在する
				if (disk[unitno]) {
					// コントローラから切り離す
					if (ctrl[i]) {
						ctrl[i]->SetUnit(j, NULL);
					}

					// ユニットを解放
					if (disk[unitno] != scsibr) {
						delete disk[unitno];
					}
				}

				// 新しいユニットの設定
				disk[unitno] = map[unitno];
			}
		}
	}

	// 全コントローラを再構成
	for (i = 0; i < CtrlMax; i++) {
		// ユニット構成を調べる
		sasi_num = 0;
		scsi_num = 0;
		for (j = 0; j < UnitNum; j++) {
			unitno = i * UnitNum + j;
			// ユニットの種類で分岐
			if (disk[unitno]) {
				if (disk[unitno]->IsSASI()) {
					// SASIドライブ数加算
					sasi_num++;
				} else {
					// SCSIドライブ数加算
					scsi_num++;
				}
			}

			// ユニットを取り外す
			if (ctrl[i]) {
				ctrl[i]->SetUnit(j, NULL);
			}
		}

		// 接続ユニットなし
		if (sasi_num == 0 && scsi_num == 0) {
			if (ctrl[i]) {
				delete ctrl[i];
				ctrl[i] = NULL;
			}
			continue;
		}

		// SASI,SCSI混在
		if (sasi_num > 0 && scsi_num > 0) {
			LogWrite(fp, "Error : SASI and SCSI can't be mixed\n");
			continue;
		}

		if (sasi_num > 0) {
			// SASIのユニットのみ

			// コントローラのタイプが違うなら解放
			if (ctrl[i] && !ctrl[i]->IsSASI()) {
				delete ctrl[i];
				ctrl[i] = NULL;
			}

			// SASIコントローラ生成
			if (!ctrl[i]) {
				ctrl[i] = new SASIDEV();
				ctrl[i]->Connect(i, bus);
			}
		} else {
			// SCSIのユニットのみ

			// コントローラのタイプが違うなら解放
			if (ctrl[i] && !ctrl[i]->IsSCSI()) {
				delete ctrl[i];
				ctrl[i] = NULL;
			}

			// SCSIコントローラ生成
			if (!ctrl[i]) {
				ctrl[i] = new SCSIDEV();
				ctrl[i]->Connect(i, bus);
			}
		}

		// 全ユニット接続
		for (j = 0; j < UnitNum; j++) {
			unitno = i * UnitNum + j;
			if (disk[unitno]) {
				// ユニット接続
				ctrl[i]->SetUnit(j, disk[unitno]);
			}
		}
	}
}

//---------------------------------------------------------------------------
//
//	コマンド処理
//
//---------------------------------------------------------------------------
BOOL ProcessCmd(FILE *fp, int id, int un, int cmd, int type, char *file)
{
	Disk *map[CtrlMax * UnitNum];
	int len;
	char *ext;
	BOOL filecheck;
	Filepath filepath;
	Disk *pUnit;

	// ユニット一覧を複写
	memcpy(map, disk, sizeof(disk));

	// IDチェック
	if (id < 0 || id >= CtrlMax) {
		LogWrite(fp, "Error : Invalid ID\n");
		return FALSE;
	}

	// ユニットチェック
	if (un < 0 || un >= UnitNum) {
		LogWrite(fp, "Error : Invalid unit number\n");
		return FALSE;
	}

	// 接続コマンド
	if (cmd == 0) {					// ATTACH
		// SASIとSCSIを見分ける
		ext = NULL;
		pUnit = NULL;
		if (type == 0) {
			// パスチェック
			if (!file) {
				return FALSE;
			}

			// 最低5文字
			len = strlen(file);
			if (len < 5) {
				return FALSE;
			}

			// 拡張子チェック
			if (file[len - 4] != '.') {
				return FALSE;
			}

			// 拡張子がSASIタイプで無ければSCSIに差し替え
			ext = &file[len - 3];
			if (_xstrcasecmp(ext, "hdf") != 0) {
				type = 1;
			}
		}

		// タイプ別のインスタンスを生成
		filecheck = TRUE;
		switch (type) {
			case 0:		// HDF
				pUnit = new SASIHD();
				break;
			case 1:		// HDS/HDN/HDI/NHD/HDA
				if (ext == NULL) {
					break;
				}
				if (_xstrcasecmp(ext, "hdn") == 0 ||
					_xstrcasecmp(ext, "hdi") == 0 ||
					_xstrcasecmp(ext, "nhd") == 0) {
					pUnit = new SCSIHD_NEC();
				} else if (_xstrcasecmp(ext, "hda") == 0) {
					pUnit = new SCSIHD_APPLE();
				} else {
					pUnit = new SCSIHD();
				}
				break;
			case 2:		// MO
				pUnit = new SCSIMO();
				if (_xstrcasecmp(file, "-") == 0 ||
					_xstrcasecmp(file, "mo") == 0) {
					filecheck = FALSE;
				}
				break;
			case 3:		// CD
				pUnit = new SCSICD();
				if (_xstrcasecmp(file, "-") == 0 ||
					_xstrcasecmp(file, "cd") == 0) {
					filecheck = FALSE;
				}
				break;
			case 4:		// BRIDGE
				pUnit = scsibr;
				filecheck = FALSE;
				break;
			default:
				LogWrite(fp,	"Error : Invalid device type\n");
				return FALSE;
		}

		// ファイルの確認を行う
		if (filecheck) {
			// パスを設定
			filepath.SetPath(file);

			// オープン
			if (!pUnit->Open(filepath)) {
				LogWrite(fp, "Error : File open error [%s]\n", file);
				delete pUnit;
				return FALSE;
			}
		}

		// ライトスルーに設定
		pUnit->SetCacheWB(FALSE);

		// 新しいユニットで置き換え
		map[id * UnitNum + un] = pUnit;

		// コントローラマッピング
		MapControler(fp, map);
		return TRUE;
	}

	// 有効なコマンドか
	if (cmd > 4) {
		LogWrite(fp, "Error : Invalid command\n");
		return FALSE;
	}

	// コントローラが存在するか
	if (ctrl[id] == NULL) {
		LogWrite(fp, "Error : No such device\n");
		return FALSE;
	}

	// ユニットが存在するか
	pUnit = disk[id * UnitNum + un];
	if (pUnit == NULL) {
		LogWrite(fp, "Error : No such device\n");
		return FALSE;
	}

	// 切断コマンド
	if (cmd == 1) {					// DETACH
		// 既存のユニットを解放
		map[id * UnitNum + un] = NULL;

		// コントローラマッピング
		MapControler(fp, map);
		return TRUE;
	}

	// MOかCDの場合だけ有効
	if (pUnit->GetID() != MAKEID('S', 'C', 'M', 'O') &&
		pUnit->GetID() != MAKEID('S', 'C', 'C', 'D')) {
		LogWrite(fp, "Error : Operation denied(Deveice isn't removable)\n");
		return FALSE;
	}

	switch (cmd) {
		case 2:						// INSERT
			// パスを設定
			filepath.SetPath(file);

			// オープン
			if (!pUnit->Open(filepath)) {
				LogWrite(fp, "Error : File open error [%s]\n", file);
				return FALSE;
			}
			break;

		case 3:						// EJECT
			pUnit->Eject(TRUE);
			break;

		case 4:						// PROTECT
			if (pUnit->GetID() != MAKEID('S', 'C', 'M', 'O')) {
				LogWrite(fp, "Error : Operation denied(Deveice isn't MO)\n");
				return FALSE;
			}
			pUnit->WriteP(!pUnit->IsWriteP());
			break;
		default:
			ASSERT(FALSE);
			return FALSE;
	}

	return TRUE;
}

//---------------------------------------------------------------------------
//
//	コマンド解析
//
//---------------------------------------------------------------------------
BOOL ParseCmd(char *argID, char *argPath)
{
	int id;
	int un;
	int type;
	int len;
	char *ext;

	if (strlen(argID) == 3 && _xstrncasecmp(argID, "id", 2) == 0) {
		// ID or idの形式

		// ID番号をチェック(0-7)
		if (argID[2] < '0' || argID[2] > '7') {
			LogWrite(stderr,
				"Error : Invalid argument(IDn n=0-7) [%c]\n", argID[2]);
			return FALSE;
		}

		// ID,ユニット確定
		id = argID[2] - '0';
		un = 0;
	} else if (_xstrncasecmp(argID, "hd", 2) == 0) {
		// HD or hdの形式

		if (strlen(argID) == 3) {
			// HD番号をチェック(0-9)
			if (argID[2] < '0' || argID[2] > '9') {
				LogWrite(stderr,
					"Error : Invalid argument(HDn n=0-15) [%c]\n", argID[2]);
				return FALSE;
			}

			// ID,ユニット確定
			id = (argID[2] - '0') / UnitNum;
			un = (argID[2] - '0') % UnitNum;
		} else if (strlen(argID) == 4) {
			// HD番号をチェック(10-15)
			if (argID[2] != '1' || argID[3] < '0' || argID[3] > '5') {
				LogWrite(stderr,
					"Error : Invalid argument(HDn n=0-15) [%c]\n", argID[2]);
				return FALSE;
			}

			// ID,ユニット確定
			id = ((argID[3] - '0') + 10) / UnitNum;
			un = ((argID[3] - '0') + 10) % UnitNum;
		} else {
			LogWrite(stderr,
				"Error : Invalid argument(IDn or HDn) [%s]\n", argID);
			return FALSE;
		}
	} else {
		LogWrite(stderr,
			"Error : Invalid argument(IDn or HDn) [%s]\n", argID);
		return FALSE;
	}

	// すでにアクティブなデバイスがあるならスキップ
	if (disk[id * UnitNum + un] &&
		!disk[id * UnitNum + un]->IsNULL()) {
		return TRUE;
	}

	// デバイスタイプを初期化
	type = -1;

	// イーサネットとホストブリッジのチェック
	if (_xstrcasecmp(argPath, "bridge") == 0) {
		type = 4;
	} else if (_xstrcasecmp(argPath, "mo") == 0) {
		// ファイル指定なしでMOとみなす
		type = 2;
	} else if (_xstrcasecmp(argPath, "cd") == 0) {
		// ファイル指定なしでCDとみなす
		type = 3;
	} else {
		// パスの長さをチェック
		len = strlen(argPath);
		if (len < 5) {
			LogWrite(stderr,
				"Error : Invalid argument(File path is short) [%s]\n",
				argPath);
			return FALSE;
		}

		// 拡張子を持っているか？
		if (argPath[len - 4] != '.') {
			LogWrite(stderr,
				"Error : Invalid argument(No extension) [%s]\n", argPath);
			return FALSE;
		}

		// タイプを決める
		ext = &argPath[len - 3];
		if (_xstrcasecmp(ext, "hdf") == 0 ||
			_xstrcasecmp(ext, "hds") == 0 ||
			_xstrcasecmp(ext, "hdn") == 0 ||
			_xstrcasecmp(ext, "hdi") == 0 ||
			_xstrcasecmp(ext, "nhd") == 0 ||
			_xstrcasecmp(ext, "hda") == 0) {
			// HD(SASI/SCSI)
			type = 0;
		} else if (strcasecmp(ext, "mos") == 0) {
			// MO
			type = 2;
		} else if (strcasecmp(ext, "iso") == 0) {
			// CD
			type = 3;
		} else {
			// タイプが判別できない
			LogWrite(stderr,
				"Error : Invalid argument(file type) [%s]\n", ext);
			return FALSE;
		}
	}

	// コマンド実行
	if (!ProcessCmd(stderr, id, un, 0, type, argPath)) {
		return FALSE;
	}

	return TRUE;
}

//---------------------------------------------------------------------------
//
//	引数処理
//
//---------------------------------------------------------------------------
BOOL ParseArgument(int argc, char* argv[])
{
	int i;
	char *argID;
	char *argPath;

	// IDとパス指定がなければ処理を中断
	if (argc < 3) {
		return TRUE;
	}

	// コマンド名をスキップ
	i = 1;
	argc--;

	// 解読開始
	while (TRUE) {
		if (argc < 2) {
			break;
		}

		argc -= 2;

		// IDとパスを取得
		argID = argv[i++];
		argPath = argv[i++];

		// 事前チェック
		if (argID[0] != '-') {
			LogWrite(stderr,
				"Error : Invalid argument(-IDn or -HDn) [%s]\n", argID);
			break;
		}
		argID++;

		// コマンド解析
		ParseCmd(argID, argPath);
	}

	return TRUE;
}

//---------------------------------------------------------------------------
//
//	設定ファイル処理
//
//---------------------------------------------------------------------------
BOOL ParseConfigFile(char *file)
{
	Fileio fio;
	char line[512];
	char argID[512];
	char argPath[512];
	int len;
	char *p;
	char *q;

	// 設定ファイルがなければ処理を中断
	if (!fio.Open(file, Fileio::ReadOnly)) {
		LogWrite(stdout, "Error : %s is not found.\n", file);
		return FALSE;
	}

	// 解読開始
	while (TRUE) {
		// 1行取得
		memset(line, 0x00, sizeof(line));
		if (!fio.ReadLine(line, sizeof(line) -1)) {
			break;
		}

		// CR/LF削除
		len = strlen(line);
		while (len > 0) {
			if (line[len - 1] != '\r' && line[len - 1] != '\n') {
				break;
			}
			line[len - 1] = '\0';
			len--;
		}

		// 空行スキップ
		if (line[0] == '\0') {
			continue;
		}

		// 空白とタブ以外の文字まで走査
		p = line;
		while (p[0]) {
			if (p[0] != ' ' && p[0] != '\t') {
				break;
			}
			p++;
		}

		// コメント行をスキップ
		if (p[0] == '#') {
			continue;
		}

		// 空白とタブまで走査
		q = p;
		while (q[0]) {
			if (q[0] == ' ' || q[0] == '\t') {
				break;
			}
			q++;
		}

		// ID確定
		memcpy(argID, p, (q - p));
		argID[(q - p)] = '\0';


		// 空白とタブ以外の文字まで走査
		p = q;
		while (p[0]) {
			if (p[0] != ' ' && p[0] != '\t') {
				break;
			}
			p++;
		}

		// 空白とタブまで走査
		q = p;
		while (q[0]) {
			if (q[0] == ' ' || q[0] == '\t') {
				break;
			}
			q++;
		}

		// パス確定
		memcpy(argPath, p, (q - p));
		argPath[(q - p)] = '\0';

		// 事前チェック
		if (argID[0] == '\0' || argPath[0] == '\0') {
			continue;
		}

		// コマンド解析
		ParseCmd(argID, argPath);
	}

	// 設定ファイルクローズ
	fio.Close();

	return TRUE;
}

//---------------------------------------------------------------------------
//
//	コントールコマンド処理
//
//---------------------------------------------------------------------------
void ParseCtrCmd(FILE *fp, char *line)
{
	int i;
	char *p;
	char *argv[5];
	int id;
	int un;
	int cmd;
	int type;
	char *file;

	// 出力先がログバッファならクリア
	if (!fp) {
		logbuf[0] = '\0';
	}

	// パラメータ取得
	p = line;

	// 改行文字を削除
	p[strlen(p) - 1] = 0;

	// シャットダウン指示(3秒後)
	if (_xstrncasecmp(p, "shutdown", 8) == 0) {
		haltreq = TRUE;
#ifdef BAREMETAL
		running = FALSE;
#else
		alarm(3);
#endif	// BAREMETAL
		return;
	}

	// サーバ終了指示(3秒後)
	if (_xstrncasecmp(p, "stop", 4) == 0) {
#ifdef BAREMETAL
		running = FALSE;
#else
		alarm(3);
#endif	// BAREMETAL
		return;
	}

	// デバイスリスト表示
	if (_xstrncasecmp(p, "list", 4) == 0) {
		ListDevice(fp);
		return;
	}

	// パラメータの分離
	argv[0] = p;
	for (i = 1; i < 5; i++) {
		// パラメータ値をスキップ
		while (*p && (*p != ' ')) {
			p++;
		}

		// スペースをNULLに置換
		while (*p && (*p == ' ')) {
			*p++ = 0;
		}

		// パラメータを見失った
		if (!*p) {
			break;
		}

		// パラメータとして認識
		argv[i] = p;
	}

	// 全パラメータの取得失敗
	if (i < 5) {
		delete[] p;
		return;
	}

	// ID,ユニット,コマンド,タイプ,ファイル
	id = atoi(argv[0]);
	un = atoi(argv[1]);
	cmd = atoi(argv[2]);
	type = atoi(argv[3]);
	file = argv[4];

	// コマンド実行
	ProcessCmd(fp, id, un, cmd, type, file);
}

#ifndef BAREMETAL
//---------------------------------------------------------------------------
//
//	モニタースレッド
//
//---------------------------------------------------------------------------
static void *MonThread(void *param)
{
	struct sockaddr_in client;
	socklen_t len; 
	int fd;
	FILE *fp;
	char buf[BUFSIZ];
	char *line;

	// CPUを固定
	FixCpu(2);

	// 実行優先順位
	SetExecPrio(PRIO_MIN);

	// 実行開始待ち
	while (!running) {
		usleep(1);
	}

	// 監視準備
	listen(monsocket, 1);

	while (running) {
		// 接続待ち
		memset(&client, 0, sizeof(client)); 
		len = sizeof(client); 
		fd = accept(monsocket, (struct sockaddr*)&client, &len);
		if (fd < 0) {
			break;
		}

		// コマンドライン取得
		fp = fdopen(fd, "r+");
		line = fgets(buf, BUFSIZ, fp);

		if (line) {
			// アイドルになるまで待つ
			while (active) {
				usleep(500 * 1000);
			}

			// コマンドライン処理
			ParseCtrCmd(fp, line);
		}

		// 接続解放
		fclose(fp);
		close(fd);
	}

	return NULL;
}
#endif	// !BAREMETAL

//---------------------------------------------------------------------------
//
//	コントローラコールバック関数
//
//---------------------------------------------------------------------------
int CtlCallback(BOOL read, int func, int phase, int len, BYTE *buf)
{
	int msglen;
	char line[BUFSIZ];

	if (read) {
		// ブリッジがデータの取得を要求している

		// 返却データクリア
		memset(buf, 0x00, len);

		// ログの転送
		msglen = strlen(logbuf);
		if (msglen > len) {
			msglen = len;
		}
		memcpy(buf, logbuf, msglen);

		return len;
	} else {
		// ブリッジがデータの処理を要求している

		// コマンドライン取得
		strcpy(line, (char*)buf);

		// コマンドライン処理
		ParseCtrCmd(NULL, line);

		return len;
	}
}

//---------------------------------------------------------------------------
//
//	主処理
//
//---------------------------------------------------------------------------
#ifdef BAREMETAL
extern "C"
int startrascsi(void)
{
	int argc = 2;
	char *argv[2];
#else
int main(int argc, char* argv[])
{
#endif	// BAREMETAL
	int i;
	int ret;
	int actid;
	DWORD now;
	BUS::phase_t phase;
	BYTE data;

#ifdef BAREMETAL
	// 設定ファイル固定
	argv[0] = (char *)"rascsi";
	argv[1] = (char *)"rascsi.ini";
#endif	// BAREMETAL

	// バナー出力
	Banner(argc, argv);

	// 初期化
	ret = 0;
	if (!Init()) {
		ret = EPERM;
		goto init_exit;
	}

	// バスリセット
	bus->Reset();

	// BUSYアサート(ホスト側を待たせるため)
	bus->SetBSY(TRUE);

	// 引数処理
	if (argc == 2) {
		if (!ParseConfigFile(argv[1])) {
			ret = EINVAL;
			goto err_exit;
		}
	} else {
		if (!ParseArgument(argc, argv)) {
			ret = EINVAL;
			goto err_exit;
		}
	}

	// デバイスリスト表示
	ListDevice(stdout);

	// BUSYネゲート(ホスト側を待たせるため)
	bus->SetBSY(FALSE);

	// コントローラとバスをリセット
	Reset();

	// CPUを固定
	FixCpu(3);

	// 実行優先順位
	SetExecPrio(PRIO_MAX);

	// 実行開始
	running = TRUE;

	// メインループ
	while (running) {
		// ワーク初期化
		actid = -1;
		phase = BUS::busfree;

		// SEL信号ポーリング
		ret = bus->PollSelectEvent();
		if (ret < 0) {
			continue;
		}

		// バスの状態取得
		bus->Aquire();

		// バスリセットを通知
		if (bus->GetRST()) {
			for (i = 0; i < CtrlMax; i++) {
				if (ctrl[i]) {
					ctrl[i]->Reset();
				}
			}
		}

		// SELECT中で無ければ中断
		if (!bus->GetSEL()) {
			continue;
		}

		// イニシエータがID設定中にアサートしている
		// 可能性があるのでBSYが解除されるまで待つ(最大3秒)
		if (bus->GetBSY()) {
			now = SysTimer::GetTimerLow();
			while ((SysTimer::GetTimerLow() - now) < 3 * 1000 * 1000) {
				bus->Aquire();
				if (!bus->GetBSY()) {
					break;
				}
			}
		}

		// ビジーまたは他のデバイスが応答したので止める
		if (bus->GetBSY() || !bus->GetSEL()) {
			continue;
		}

		// 全コントローラに通知
		data = bus->GetDAT();
		for (i = 0; i < CtrlMax; i++) {
			if (!ctrl[i] || (data & (1 << i)) == 0) {
				continue;
			}

			// セレクションフェーズに移行したターゲットを探す
			if (ctrl[i]->Process() == BUS::selection) {
				// ターゲットのIDを取得
				actid = i;

				// セレクションフェーズ
				phase = BUS::selection;
				break;
			}
		}

		// セレクションフェーズが開始されていなければバスの監視へ戻る
		if (phase != BUS::selection) {
			continue;
		}

		// ターゲット走行開始
		active = TRUE;

		// バスフリーになるまでループ
		while (running) {
			// ターゲット駆動
			phase = ctrl[actid]->Process();

			// バスフリーになったら終了
			if (phase == BUS::busfree) {
				break;
			}
		}

		// ターゲット走行終了
		active = FALSE;
	}

err_exit:
	// クリーンアップ
	Cleanup();

init_exit:
#if !defined(BAREMETAL)
	if (haltreq) {
		system("halt");
	}
	exit(ret);
#else
	return ret;
#endif	// BAREMETAL
}
