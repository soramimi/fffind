#include <cstdint>
#include <cstring>
#include <unistd.h>
#include <string>
#include <vector>
#include <arpa/inet.h>
#include <net/if.h>
#include <netinet/in.h>
#include <sys/ioctl.h>

struct Network {
	uint32_t addr = 0;
	uint32_t mask = 0;
};


struct FlashForgePrnter {
	uint32_t addr = 0;
	uint16_t port = 0;
	std::string name;
};

/**
 * @brief 指定されたネットワーク上のプリンタを検索
 * @param sock UDPソケット
 * @param nw 検索対象のネットワーク
 * @param out プリンタリスト
 */
void find(int sock, Network const &nw, std::vector<FlashForgePrnter> *out)
{
	out->clear();

	uint8_t buf[2048];
	struct timeval tv;

	// 1秒でタイムアウトする
	tv.tv_sec = 1;
	tv.tv_usec = 0;

	const uint32_t multicast = 0xe1000009; // 225.0.0.9
	const uint16_t port = 19000;
	{
		struct sockaddr_in addr;
		// 待ち受けポート番号を19000にするためにbind()する
		addr.sin_family = AF_INET;
		addr.sin_port = htons(port);
		addr.sin_addr.s_addr = INADDR_ANY;
		bind(sock, (struct sockaddr *)&addr, sizeof(addr));
		// 問い合わせパケットを送信
		{
			buf[0] = (nw.addr >> 24) & 0xff;
			buf[1] = (nw.addr >> 16) & 0xff;
			buf[2] = (nw.addr >> 8) & 0xff;
			buf[3] = (nw.addr) & 0xff;
			buf[4] = (port >> 8) & 0xff;
			buf[5] = (port) & 0xff;
			buf[6] = 0;
			buf[7] = 0;
			addr.sin_addr.s_addr = htonl(multicast);
			sendto(sock, buf, 8, 0, (struct sockaddr *)&addr, sizeof(addr));
		}
	}
	// マルチキャスト受信設定
	{
		struct ip_mreq mreq = {};
		mreq.imr_interface.s_addr = INADDR_ANY;
		mreq.imr_multiaddr.s_addr = htonl(multicast);
		setsockopt(sock, IPPROTO_IP, IP_ADD_MEMBERSHIP, (void *)&mreq, sizeof(mreq));
	}
	// 応答パケットを受信
	while (1) {
		fd_set fds;
		FD_ZERO(&fds);
		FD_SET(sock, &fds);
		// fdsに設定されたソケットが読み込み可能になるまで待つ
		int n = select(sock + 1, &fds, nullptr, nullptr, &tv);
		if (n == 0) break;

		if (FD_ISSET(sock, &fds)) { // 読み取り可能状態になった？
			struct sockaddr_in senderinfo;
			memset(buf, 0, sizeof(buf));
			// UDPソケットからデータを受信
			socklen_t addrlen = sizeof(senderinfo);
			int len = recvfrom(sock, buf, sizeof(buf), 0, (struct sockaddr *)&senderinfo, &addrlen);
			if (len == 140) {
				FlashForgePrnter printer;
				uint32_t mcast = 0;
				mcast = (buf[128 + 0] << 24) | (buf[128 + 1] << 16) | (buf[128 + 2] << 8) | buf[128 + 3];
				if (mcast == multicast) {
					// プリンタ名
					int i;
					for (i = 0; i < 128 && buf[i]; i++);
					printer.name.assign((char const *)buf, i);
					// IPアドレス
					printer.addr = ntohl(senderinfo.sin_addr.s_addr);
					// ポート
					printer.port = (buf[128 + 4] << 8) | buf[128 + 5];
					// 検索結果を保存
					out->push_back(printer);
					// 情報表示
					printf("  Found: %d.%d.%d.%d:%d \"%s\"\n"
						   , (printer.addr >> 24) & 0xff, (printer.addr >> 16) & 0xff, (printer.addr >> 8) & 0xff, printer.addr & 0xff
						   , printer.port
						   , printer.name.c_str()
						   );
				}
			}
		}
	}
}

int main(int argc, char **argv)
{
	in_addr _addr;

	if (argc != 2) {
		fprintf(stderr, "FlashForge Finder\n");
		fprintf(stderr, "usage: %s [network address]\n", argv[0]);
		fprintf(stderr, "  e.g. %s 192.168.0.0\n", argv[0]);
		return 1;
	}

	_addr.s_addr = ntohl(inet_addr(argv[1]));

	std::vector<FlashForgePrnter> printers;

	std::vector<std::string> netifs;

	// ネッチワークインターフェースを列挙
	{
#define MAX_IFR 100
		struct ifreq ifr[MAX_IFR];
		struct ifconf ifc;

		int sock = socket(AF_INET, SOCK_DGRAM, 0);

		// データを受け取る部分の長さ
		ifc.ifc_len = sizeof(ifr);

		// kernelからデータを受け取る部分を指定
		ifc.ifc_ifcu.ifcu_buf = (char *)ifr;

		ioctl(sock, SIOCGIFCONF, &ifc);

		// kernelから帰ってきた数を計算
		int n = ifc.ifc_len / sizeof(struct ifreq);

		for (int i = 0; i < n; i++) {
			netifs.push_back(ifr[i].ifr_name); // インターフェース名
		}
	}

	// 見つかった全てのインターフェースを調査
	for (auto const &netif : netifs) {
		struct ifreq ifr;
		Network nw;

		int sock = socket(AF_INET, SOCK_DGRAM, 0);

		// IPv4のIPアドレスを取得したい
		ifr.ifr_addr.sa_family = AF_INET;
		strncpy(ifr.ifr_name, netif.c_str(), IFNAMSIZ - 1);

		// IPアドレスを取得
		ioctl(sock, SIOCGIFADDR, &ifr);
		nw.addr = ntohl(((struct sockaddr_in *)&ifr.ifr_addr)->sin_addr.s_addr);

		// サブネットマスクを取得
		ioctl(sock, SIOCGIFNETMASK, &ifr);
		nw.mask = ntohl(((struct sockaddr_in *)&ifr.ifr_netmask)->sin_addr.s_addr);

		// 検索対象のインターフェースか？
		if ((_addr.s_addr & nw.mask) == (nw.addr & nw.mask)) {
			// インターフェース情報を表示
			printf("%d.%d.%d.%d/%d.%d.%d.%d %s\n"
				   , (nw.addr >> 24) & 0xff, (nw.addr >> 16) & 0xff, (nw.addr >> 8) & 0xff, nw.addr & 0xff
				   , (nw.mask >> 24) & 0xff, (nw.mask >> 16) & 0xff, (nw.mask >> 8) & 0xff, nw.mask & 0xff
				   , netif.c_str()
				   );
			// プリンタを検索
			std::vector<FlashForgePrnter> v;
			find(sock, nw, &v);
			printers.insert(printers.end(), v.begin(), v.end());
		}

		close(sock);
	}

	return 0;
}
