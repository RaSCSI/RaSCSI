//---------------------------------------------------------------------------
//
//	SCSI Target Emulator RaSCSI (*^..^*)
//	for Raspberry Pi
//	Powered by XM6 TypeG Technology.
//
//	Copyright (C) 2016-2021 GIMONS(Twitter:@kugimoto0715)
//
//	Imported NetBSD support and some optimisation patch by Rin Okuyama.
//
//	[ ネットワークドライバ ]
//
//---------------------------------------------------------------------------

#include "os.h"
#include "rascsi.h"
#include "netdriver.h"

//===========================================================================
//
//	ネットワークドライバ
//
//===========================================================================
static NetDriver *self;

//---------------------------------------------------------------------------
//
//	コールバック関数
//
//---------------------------------------------------------------------------
int NetCallback(BOOL read, int func, int phase, int len, BYTE *buf)
{
	ASSERT(self);

	return self->Process(read, func, phase, len, buf);
}

//---------------------------------------------------------------------------
//
//	コンストラクタ
//
//---------------------------------------------------------------------------
NetDriver::NetDriver()
{
	// TAPドライバ生成
	tap = new CTapDriver();
	m_bTapEnable = tap->Init();

	// MACアドレスを生成
	memset(mac_addr, 0x00, 6);
	if (m_bTapEnable) {
		tap->GetMacAddr(mac_addr);
		mac_addr[5]++;
	}

	// パケット受信フラグオフ
	packet_enable = FALSE;

	// 自身を保存
	self = this;
}

//---------------------------------------------------------------------------
//
//	デストラクタ
//
//---------------------------------------------------------------------------
NetDriver::~NetDriver()
{
	// TAPドライバ解放
	if (tap) {
		tap->Cleanup();
		delete tap;
	}
}

//---------------------------------------------------------------------------
//
//	実行
//
//---------------------------------------------------------------------------
int FASTCALL NetDriver::Process(
	BOOL read, int func, int phase, int len, BYTE *buf)
{
	int i;
	int total_len;

	ASSERT(this);

	// TAP無効なら処理しない
	if (!m_bTapEnable) {
		return 0;
	}

	if (read) {
		switch (func) {
			case 0:		// MACアドレス取得
				return GetMacAddr(buf);

			case 1:		// 受信パケット取得(サイズ/バッファ別)
				if (phase == 0) {
					// パケットサイズ取得
					ReceivePacket();
					buf[0] = (BYTE)(packet_len >> 8);
					buf[1] = (BYTE)packet_len;
					return 2;
				} else {
					// パケットデータ取得
					GetPacketBuf(buf);
					return packet_len;
				}

			case 2:		// 受信パケット取得(サイズ＋バッファ同時)
				ReceivePacket();
				buf[0] = (BYTE)(packet_len >> 8);
				buf[1] = (BYTE)packet_len;
				GetPacketBuf(&buf[2]);
				return packet_len + 2;

			case 3:		// 複数パケット同時取得(サイズ＋バッファ同時)
				// パケット数の上限は現状10個に決め打ち
				// これ以上増やしてもあまり速くならない?
				total_len = 0;
				for (i = 0; i < 10; i++) {
					ReceivePacket();
					*buf++ = (BYTE)(packet_len >> 8);
					*buf++ = (BYTE)packet_len;
					total_len += 2;
					if (packet_len == 0)
						break;
					GetPacketBuf(buf);
					buf += packet_len;
					total_len += packet_len;
				}
				return total_len;
		}
	} else {
		switch (func) {
			case 0:		// MACアドレス設定
				SetMacAddr(buf);
				return len;

			case 1:		// パケット送信
				SendPacket(buf, len);
				return len;
		}
	}

	// エラー
	ASSERT(FALSE);
	return 0;
}

//---------------------------------------------------------------------------
//
//	MACアドレス取得
//
//---------------------------------------------------------------------------
int FASTCALL NetDriver::GetMacAddr(BYTE *mac)
{
	ASSERT(this);
	ASSERT(mac);

	memcpy(mac, mac_addr, 6);
	return 6;
}

//---------------------------------------------------------------------------
//
//	MACアドレス設定
//
//---------------------------------------------------------------------------
void FASTCALL NetDriver::SetMacAddr(BYTE *mac)
{
	ASSERT(this);
	ASSERT(mac);

	memcpy(mac_addr, mac, 6);
}

//---------------------------------------------------------------------------
//
//	パケット受信
//
//---------------------------------------------------------------------------
void FASTCALL NetDriver::ReceivePacket()
{
	static const BYTE bcast_addr[6] = { 0xff, 0xff, 0xff, 0xff, 0xff, 0xff };

	ASSERT(this);
	ASSERT(tap);

	// 前のパケットが受信されていない
	if (packet_enable) {
		return;
	}

	// パケット受信
	packet_len = tap->Rx(packet_buf);

	// 受信パケットか検査
	if (memcmp(packet_buf, mac_addr, 6) != 0) {
		if (memcmp(packet_buf, bcast_addr, 6) != 0) {
			packet_len = 0;
			return;
		}
	}

	// バッファサイズを越えたら捨てる
	if (packet_len > 2048) {
		packet_len = 0;
		return;
	}

	// 受信バッファに格納
	if (packet_len > 0) {
		packet_enable = TRUE;
	}
}

//---------------------------------------------------------------------------
//
//	パケット取得
//
//---------------------------------------------------------------------------
void FASTCALL NetDriver::GetPacketBuf(BYTE *buf)
{
	int len;

	ASSERT(this);
	ASSERT(tap);
	ASSERT(buf);

	// サイズ制限
	len = packet_len;
	if (len > 2048) {
		len = 2048;
	}

	// コピー
	memcpy(buf, packet_buf, len);

	// 受信済み
	packet_enable = FALSE;
}

//---------------------------------------------------------------------------
//
//	パケット送信
//
//---------------------------------------------------------------------------
void FASTCALL NetDriver::SendPacket(BYTE *buf, int len)
{
	ASSERT(this);
	ASSERT(tap);
	ASSERT(buf);

	tap->Tx(buf, len);
}

//===========================================================================
//
//	TAPドライバ
//
//===========================================================================

//---------------------------------------------------------------------------
//
//	コンストラクタ
//
//---------------------------------------------------------------------------
CTapDriver::CTapDriver()
{
	// 初期化
	m_hTAP = -1;
	memset(&m_MacAddr, 0, sizeof(m_MacAddr));
}

//---------------------------------------------------------------------------
//
//	初期化
//
//---------------------------------------------------------------------------
#ifdef __linux__
BOOL FASTCALL CTapDriver::Init()
{
	char dev[IFNAMSIZ] = "ras0";
	struct ifreq ifr;
	int ret;

	ASSERT(this);

	// TAPデバイス初期化
	if ((m_hTAP = open("/dev/net/tun", O_RDWR)) < 0) {
		printf("Error: can't open tun\n");
		return FALSE;
	}

	// IFF_NO_PI for no extra packet information
	memset(&ifr, 0, sizeof(ifr));
	ifr.ifr_flags = IFF_TAP | IFF_NO_PI;
	strncpy(ifr.ifr_name, dev, IFNAMSIZ);
	if ((ret = ioctl(m_hTAP, TUNSETIFF, (void *)&ifr)) < 0) {
		printf("Error: can't ioctl TUNSETIFF\n");
		close(m_hTAP);
		return FALSE;
	}

	// MACアドレス取得
	ifr.ifr_addr.sa_family = AF_INET;
	if ((ret = ioctl(m_hTAP, SIOCGIFHWADDR, &ifr)) < 0) {
		printf("Error: can't ioctl SIOCGIFHWADDR\n");
		close(m_hTAP);
		return FALSE;
	}

	// MACアドレス保存
	memcpy(m_MacAddr, ifr.ifr_hwaddr.sa_data, sizeof(m_MacAddr));
	return TRUE;
}
#endif // __linux__

#ifdef __NetBSD__
BOOL FASTCALL CTapDriver::Init()
{
	struct ifreq ifr;
	struct ifaddrs *ifa, *a;
	
	ASSERT(this);

	// TAPデバイス初期化
	if ((m_hTAP = open("/dev/tap", O_RDWR)) < 0) {
		printf("Error: can't open tap\n");
		return FALSE;
	}

	// デバイス名取得
	if (ioctl(m_hTAP, TAPGIFNAME, (void *)&ifr) < 0) {
		printf("Error: can't ioctl TAPGIFNAME\n");
		close(m_hTAP);
		return FALSE;
	}

	// MACアドレス取得
	if (getifaddrs(&ifa) == -1) {
		printf("Error: can't getifaddrs\n");
		close(m_hTAP);
		return FALSE;
	}
	for (a = ifa; a != NULL; a = a->ifa_next)
		if (strcmp(ifr.ifr_name, a->ifa_name) == 0 &&
			a->ifa_addr->sa_family == AF_LINK)
			break;
	if (a == NULL) {
		printf("Error: can't get MAC address\n");
		close(m_hTAP);
		return FALSE;
	}

	// MACアドレス保存
	memcpy(m_MacAddr, LLADDR((struct sockaddr_dl *)a->ifa_addr),
		sizeof(m_MacAddr));
	freeifaddrs(ifa);

	printf("Tap device : %s\n", ifr.ifr_name);

	return TRUE;
}
#endif // __NetBSD__

//---------------------------------------------------------------------------
//
//	クリーンアップ
//
//---------------------------------------------------------------------------
void FASTCALL CTapDriver::Cleanup()
{
	ASSERT(this);

	// TAPデバイス解放
	if (m_hTAP != -1) {
		close(m_hTAP);
		m_hTAP = -1;
	}
}

//---------------------------------------------------------------------------
//
//	MACアドレス取得
//
//---------------------------------------------------------------------------
void FASTCALL CTapDriver::GetMacAddr(BYTE *mac)
{
	ASSERT(this);
	ASSERT(mac);

	memcpy(mac, m_MacAddr, sizeof(m_MacAddr));
}

//---------------------------------------------------------------------------
//
//	受信
//
//---------------------------------------------------------------------------
int FASTCALL CTapDriver::Rx(BYTE *buf)
{
	struct pollfd fds;
	DWORD dwReceived;

	ASSERT(this);
	ASSERT(m_hTAP != -1);

	// 受信可能なデータがあるか調べる
	fds.fd = m_hTAP;
	fds.events = POLLIN | POLLERR;
	fds.revents = 0;
	poll(&fds, 1, 0);
	if (!(fds.revents & POLLIN)) {
		return 0;
	}

	// 受信
	dwReceived = read(m_hTAP, buf, ETH_FRAME_LEN);
	if (dwReceived == (DWORD)-1) {
		return 0;
	}

	// 受信が有効であれば
	if (dwReceived > 0) {
		// FCSを除く最小フレームサイズ(60バイト)にパディング
		if (dwReceived < 60) {
			memset(buf + dwReceived, 0, 60 - dwReceived);
			dwReceived = 60;
		}

		// ダミーのFCSを付加する
		memset(buf + dwReceived, 0, 4);
		dwReceived += 4;
	}

	// バイト数を返却
	return dwReceived;
}

//---------------------------------------------------------------------------
//
//	送信
//
//---------------------------------------------------------------------------
int FASTCALL CTapDriver::Tx(BYTE *buf, int len)
{
	ASSERT(this);
	ASSERT(m_hTAP != -1);

	// 送信開始
	return write(m_hTAP, buf, len);
}