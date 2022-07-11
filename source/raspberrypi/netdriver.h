//---------------------------------------------------------------------------
//
//	SCSI Target Emulator RaSCSI (*^..^*)
//	for Raspberry Pi
//	Powered by XM6 TypeG Technology.
//
//	Copyright (C) 2016-2021 GIMONS(Twitter:@kugimoto0715)
//	Imported NetBSD support and some optimisation patch by Rin Okuyama.
//
//	[ ネットワークドライバ ]
//
//---------------------------------------------------------------------------

#if !defined(cnetdriver_h)
#define cnetdriver_h

class CTapDriver;
class NetDriver
{
public:
	// 基本ファンクション
	NetDriver();
										// コンストラクタ
	virtual ~NetDriver();
										// デストラクタ

	int FASTCALL IsNetEnable() { return m_bTapEnable; }
										// ネットワーク機能有効取得
	int FASTCALL Process(BOOL read, int func, int phase, int len, BYTE *buf);
										// 実行
	int FASTCALL GetMacAddr(BYTE *buf);
										// MACアドレス取得
	void FASTCALL SetMacAddr(BYTE *buf);
										// MACアドレス設定
	void FASTCALL ReceivePacket();
										// パケット受信
	void FASTCALL GetPacketBuf(BYTE *buf);
										// パケット取得
	void FASTCALL SendPacket(BYTE *buf, int len);
										// パケット送信

	CTapDriver *tap;
										// TAPドライバ
	BOOL m_bTapEnable;
										// TAP有効フラグ
	BYTE mac_addr[6];
										// MACアドレス
	int packet_len;
										// 受信パケットサイズ
	BYTE packet_buf[0x1000];
										// 受信パケットバッファ
	BOOL packet_enable;
										// 受信パケット有効
};

//===========================================================================
//
//	Tapドライバ
//
//===========================================================================
#ifndef ETH_FRAME_LEN
#define ETH_FRAME_LEN 1514
#endif

class CTapDriver
{
public:
	// 基本ファンクション
	CTapDriver();
										// コンストラクタ
	BOOL FASTCALL Init();
										// 初期化
	void FASTCALL Cleanup();
										// クリーンアップ
	void FASTCALL GetMacAddr(BYTE *mac);
										// MACアドレス取得
	int FASTCALL Rx(BYTE *buf);
										// 受信
	int FASTCALL Tx(BYTE *buf, int len);
										// 送信

private:
	BYTE m_MacAddr[6];
										// MACアドレス
	BOOL m_bTxValid;
										// 送信有効フラグ
	int m_hTAP;
										// ディスクプリタ
};

#endif	// cnetdriver_h
