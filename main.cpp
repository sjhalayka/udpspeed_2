#include <winsock2.h>
#include <Ws2tcpip.h>
#include <windows.h>
#pragma comment(lib, "ws2_32")

#include <iostream>
#include <string>
#include <vector>
#include <set>
#include <map>
#include <sstream>
#include <chrono>
using namespace std;


SOCKET udp_socket = INVALID_SOCKET;
enum program_mode { talk_mode, listen_mode };


void print_usage(void)
{
	cout << "  USAGE:" << endl;
	cout << "    Listen mode:" << endl;
	cout << "      udpspeed PORT_NUMBER" << endl;
	cout << endl;
	cout << "    Talk mode:" << endl;
	cout << "      udpspeed TARGET_HOST PORT_NUMBER" << endl;
	cout << endl;
	cout << "    ie:" << endl;
	cout << "      Listen mode: udpspeed 1920" << endl;
	cout << "      Talk mode:   udpspeed www 342" << endl;
	cout << "      Talk mode:   udpspeed 127.0.0.1 950" << endl;
	cout << endl;
}

bool verify_port(const string& port_string, unsigned long int& port_number)
{
	for (size_t i = 0; i < port_string.length(); i++)
	{
		if (!isdigit(port_string[i]))
		{
			cout << "  Invalid port: " << port_string << endl;
			cout << "  Ports are specified by numerals only." << endl;
			return false;
		}
	}

	istringstream iss(port_string);
	iss >> port_number;

	if (port_string.length() > 5 || port_number > 65535 || port_number == 0)
	{
		cout << "  Invalid port: " << port_string << endl;
		cout << "  Port must be in the range of 1-65535" << endl;
		return false;
	}

	return true;
}

bool init_winsock(void)
{
	WSADATA wsa_data;
	WORD ver_requested = MAKEWORD(2, 2);

	if (WSAStartup(ver_requested, &wsa_data))
	{
		cout << "Could not initialize Winsock 2.2.";
		return false;
	}

	if (LOBYTE(wsa_data.wVersion) != 2 || HIBYTE(wsa_data.wVersion) != 2)
	{
		cout << "Required version of Winsock (2.2) not available.";
		return false;
	}

	return true;
}

bool init_options(const int& argc, char** argv, enum program_mode& mode, string& target_host_string, long unsigned int& port_number)
{
	if (!init_winsock())
		return false;

	string port_string = "";

	if (2 == argc)
	{
		mode = listen_mode;
		port_string = argv[1];
	}
	else if (3 == argc)
	{
		mode = talk_mode;
		target_host_string = argv[1];
		port_string = argv[2];
	}
	else
	{
		print_usage();
		return false;
	}

	cout.setf(ios::fixed, ios::floatfield);
	cout.precision(2);

	return verify_port(port_string, port_number);
}

void cleanup(void)
{
	// if the socket is still open, close it
	if (INVALID_SOCKET != udp_socket)
		closesocket(udp_socket);

	// shut down winsock
	WSACleanup();
}


class recv_stats
{
public:

	long long unsigned int total_elapsed_ticks = 0;
	long long unsigned int total_bytes_received = 0;
	long long unsigned int last_reported_at_ticks = 0;
	long long unsigned int last_reported_total_bytes_received = 0;

	double record_bps = 0;
};


int main(int argc, char** argv)
{
	cout << endl << "udpspeed_2 1.0 - UDP speed tester" << endl << "Copyright 2021, Shawn Halayka" << endl << endl;

	program_mode mode = listen_mode;

	string target_host_string = "";
	long unsigned int port_number = 0;

	const long unsigned int tx_buf_size = 1450;
	vector<char> tx_buf(tx_buf_size);

	const long unsigned int rx_buf_size = 8196;
	vector<char> rx_buf(rx_buf_size);

	if (!init_options(argc, argv, mode, target_host_string, port_number))
	{
		cleanup();
		return 1;
	}

	if (talk_mode == mode)
	{
		cout << "  Sending on port " << port_number << " - CTRL+C to exit." << endl;

		struct addrinfo hints;
		struct addrinfo* result;

		memset(&hints, 0, sizeof(struct addrinfo));
		hints.ai_family = AF_INET;
		hints.ai_socktype = SOCK_DGRAM;
		hints.ai_flags = 0;
		hints.ai_protocol = IPPROTO_UDP;

		ostringstream oss;
		oss << port_number;

		if (0 != getaddrinfo(target_host_string.c_str(), oss.str().c_str(), &hints, &result))
		{
			cout << "  getaddrinfo error." << endl;
			freeaddrinfo(result);
			cleanup();
			return 2;
		}

		if (INVALID_SOCKET == (udp_socket = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP)))
		{
			cout << "  Could not allocate a new socket." << endl;
			freeaddrinfo(result);
			cleanup();
			return 3;
		}

		while (1)
		{
			if (SOCKET_ERROR == (sendto(udp_socket, &tx_buf[0], tx_buf_size, 0, result->ai_addr, sizeof(struct sockaddr))))
			{
				cout << "  Socket sendto error." << endl;
				freeaddrinfo(result);
				cleanup();
				return 4;
			}
		}

		freeaddrinfo(result);
	}
	else if (listen_mode == mode)
	{
		cout << "  Listening on UDP port " << port_number << " - CTRL+C to exit." << endl;

		struct sockaddr_in my_addr;
		struct sockaddr_in their_addr;
		int addr_len = 0;

		my_addr.sin_family = AF_INET;
		my_addr.sin_port = htons((unsigned short int)port_number);
		my_addr.sin_addr.s_addr = INADDR_ANY;
		memset(&(my_addr.sin_zero), '\0', 8);
		addr_len = sizeof(struct sockaddr);

		if (INVALID_SOCKET == (udp_socket = socket(AF_INET, SOCK_DGRAM, 0)))
		{
			cout << "  Could not allocate a new socket." << endl;
			cleanup();
			return 5;
		}

		if (SOCKET_ERROR == bind(udp_socket, (struct sockaddr*) & my_addr, sizeof(struct sockaddr)))
		{
			cout << "  Could not bind socket to port " << port_number << "." << endl;
			cleanup();
			return 6;
		}

		map<string, recv_stats> senders;

		while (1)
		{
			std::chrono::high_resolution_clock::time_point start_loop_ticks = std::chrono::high_resolution_clock::now();
			
			long unsigned int temp_bytes_received = 0;
			string ip_address;
			
			if (SOCKET_ERROR == (temp_bytes_received = recvfrom(udp_socket, &rx_buf[0], rx_buf_size, 0, reinterpret_cast<struct sockaddr*>(&their_addr), &addr_len)))
			{
				cout << "  Socket recvfrom error." << endl;
				cleanup();
				return 7;
			}
			else
			{
				ostringstream oss;
				oss << static_cast<int>(their_addr.sin_addr.S_un.S_un_b.s_b1) << ".";
				oss << static_cast<int>(their_addr.sin_addr.S_un.S_un_b.s_b2) << ".";
				oss << static_cast<int>(their_addr.sin_addr.S_un.S_un_b.s_b3) << ".";
				oss << static_cast<int>(their_addr.sin_addr.S_un.S_un_b.s_b4);

				ip_address = oss.str();
				senders[ip_address].total_bytes_received += temp_bytes_received;
			}

			std::chrono::high_resolution_clock::time_point end_loop_ticks = std::chrono::high_resolution_clock::now();
			std::chrono::duration<float, std::milli> elapsed = end_loop_ticks - start_loop_ticks;
			senders[ip_address].total_elapsed_ticks += static_cast<unsigned long long>(elapsed.count());

			if (senders[ip_address].total_elapsed_ticks >= senders[ip_address].last_reported_at_ticks + 1000)
			{
				long long unsigned int bytes_sent_received_between_reports = senders[ip_address].total_bytes_received - senders[ip_address].last_reported_total_bytes_received;

				double bytes_per_second = static_cast<double>(bytes_sent_received_between_reports) / ((static_cast<double>(senders[ip_address].total_elapsed_ticks) - static_cast<double>(senders[ip_address].last_reported_at_ticks)) / 1000.0);

				if (bytes_per_second > senders[ip_address].record_bps)
					senders[ip_address].record_bps = bytes_per_second;

				senders[ip_address].last_reported_at_ticks = senders[ip_address].total_elapsed_ticks;
				senders[ip_address].last_reported_total_bytes_received = senders[ip_address].total_bytes_received;

				static const double mbits_factor = 8.0 / (1024.0 * 1024.0);

				cout << "  " << ip_address << " -- " << bytes_per_second * mbits_factor << " Mbit/s, Record: " << senders[ip_address].record_bps * mbits_factor << " Mbit/s" << endl;
			}
		}
	}

	cleanup();

	return 0;
}
