#include <iostream>
#include <string>
#include <winsock.h>
#include <sstream>
#include <fstream>
#include <filesystem>
#include <vector>
#pragma comment(lib, "wsock32.lib")

//#define SERVER_PORT 5000
#define BUF_SIZE 2000

using namespace std;

void ExitWithError(const string& message) 
{
	cout << message << "Error code:" << WSAGetLastError() << endl;
	WSACleanup();
	exit(1);
}

//mora da se koristi jer send() ne garantuje da ce se poslati svi bajtovi
int sendAll(SOCKET s, const char* data, int len)
{
	int total = 0; //koliko je bajtova do sada uspesno poslato

	while (total < len) //dok se svi bajtovi ne posalju
	{
		int n = send(s, data + total, len - total, 0); //data+total - odakle pocinje slanje //len - total koliko podataka se salje

		if (n == SOCKET_ERROR)
			return SOCKET_ERROR;

		total += n;
	}

	return total;
}

bool RecvHttpHeader(SOCKET s, string& header)
{
	header.clear();
	char buff[512];

	while (true)
	{
		int n = recv(s, buff, sizeof(buff), 0); //poruka je u baferu, velicina poruke je n

		if (n <= 0)
			return false;

		header.append(buff, buff + n); //smesti poruku u header

		if (header.find("\r\n\r\n") != string::npos) //find() vraca neki broj ako nadje, ako ne nadje vraca ::npos
			return true; //imamo ceo http zahtev

		if (header.size() > 8192) //zastita da neko ne salje beskrajno
			return false; 
	}
}

//odgovori 400/404/500 kao tekst
void SendError(SOCKET s, int code, string& reason)
{
	string body = to_string(code) + reason;

	ostringstream oss;
	oss << "HTTP/1.1 " << code << " " << reason << "\r\n"
		<< "Connection: close\r\n"
		<< "Content-Length: " << body.size() << "\r\n"
		<< "Content-type: text/html"
		<< "\r\n\r\n"
		<< body;

	string recp = oss.str();

	sendAll(s, recp.c_str(), (int)recp.size());
}

bool CheckFirstLine(const string& header, string& first, string& second, string& third) //treba & ? Ali javlja se sintaksna greska..
{
	size_t line = header.find("\r\n");
	if (line == string::npos)
		return false;

	string firstLine = header.substr(0, line); //izdvoji prvu liniju

	istringstream iss(firstLine);
	if (!(iss >> first >> second >> third))
		return false;

	return true;
}

bool MapUrlToFile(const string& path, string& filePath)
{
	//mora da pocne sa /
	if (path.empty() || path[0] != '/')
		return false;

	//zabrani ..
	if (path.find("..") != string::npos)
		return false;

	//mora .png
	if (path.size() < 4 || path.substr(path.size() - 4) != ".png")
		return false;

	//skini prvi /
	string relativeName = path.substr(1);

	//napravi putanju
	filePath = ".\\wwwroot\\" + relativeName;

	return true;
}

bool FileExists(string& filePath)
{
	ifstream f(filePath, ios::binary);
	return f.good();
}

bool ReadFileBytes(string& filePath, vector<char>& bytes)
{
	ifstream f(filePath, ios::binary);

	if (!f)
		return false;

	f.seekg(0, ios::end); //pomeri poziciju na kraj fajla

	streamsize sz = f.tellg(); //vrati trenutnu poziciju - posto smo na kraju vratice broj bajta u fajlu

	if (sz < 0)
		return false;

	bytes.resize((size_t)sz); //vector dobija velicinu sz
	f.seekg(0, ios::beg); //vrati pokazivac na pocetak

	if (sz > 0) //ako je prazan nema sta da se cita, ako ima procita sz bajtova i smesti u vector
	{
		f.read(bytes.data(), sz);
		if (!f) return false;
	}

	return true;
}

void sendpng(SOCKET s, const vector<char>& bytes)
{
	ostringstream oss;
	oss << "HTTP/1.1 200 OK\r\n"
		<< "Connection: close\r\n"
		<< "Content-Length: " << bytes.size() << "\r\n"
		<< "Content-Type: text/html\r\n"
		<< "\r\n";

	string header = oss.str();

	sendAll(s, header.data(), (int)header.size());

	if (!bytes.empty())
		sendAll(s, bytes.data(), (int)bytes.size());

}

int main() 
{
	WSAData wsa;
	SOCKET listensock;

	// Inicijalizacija winsoketa
	if (WSAStartup(0x0202, &wsa) != 0)
		ExitWithError("Startup failed.");

	// Kreiranje listen soketa
	if ((listensock = socket(AF_INET, SOCK_STREAM, 0)) == INVALID_SOCKET)
		ExitWithError("Listening socket not created");


	sockaddr_in server;
	server.sin_family = AF_INET;
	server.sin_addr.s_addr = INADDR_ANY; //vezi soket za sve interfejse racunara

	int serverPort;

	cout << "Unesite server port: ";
	cin >> serverPort;

	server.sin_port = htons(serverPort); //htons = host to network short. Bez htons, na nekim arhitekturama bi port ispao pogrešan

	//bind znači: “pričvrsti ovaj socket na ovu adresu i port”. Greska ce se javiti najverovatnije ako je port zauzet.
	if (bind(listensock, (struct sockaddr*)&server, sizeof(server)) == SOCKET_ERROR) 
		ExitWithError("bind() failed!");

	//osluskivanje
	if (listen(listensock, 3) < 0) //3 je backlog: koliko konekcija može da “čeka u redu” dok ti ne stigneš da ih accept()-uješ
		ExitWithError("listen() failed! ");//Ako 5 klijenata pokuša da se poveže odjednom, 3 mogu da čekaju, ostali mogu dobiti odbijanje

	//while(true) + accept() — prihvatanje klijenta (TCP konekcija)
	while (true)
	{
		sockaddr_in client;
		int sz = sizeof(client);
		SOCKET clientSock;
		//accept uspostavlja vezu sa klijentom. Vraca novi soket i taj soket predstavlja klijenta. listensock idalje slusa nove klijente
		if ((clientSock = accept(listensock, (struct sockaddr*)&client, &sz)) < 0) 
			ExitWithError("accept failed! ");

		// 1) PRIMI HTTP HEADER. Ako nema znaka \r\n\r\n onda to nije validna http poruka jer svai http ima ovo(za razdvajanje headera od body-ja)

		string header; //f-ja ce da ga napuni podacima
		if (!RecvHttpHeader(clientSock, header))
		{
			// Nismo uspeli da procitamo korektan header → 400
			string str = "Bad request";
			SendError(clientSock, 400, str);
			closesocket(clientSock);
			continue;
		}

		//2) Proveri da li je prva linija poruke u http formatu. Ako nije - onda to nije validna http poruka.
		string method, path, version;
		if (!CheckFirstLine(header, method, path, version))
		{
			string msg = "Bad request";
			SendError(clientSock, 400, msg);
			closesocket(clientSock);
			continue;
		}

		//3) Proveri verziju
		if (version != "HTTP/1.0" && version != "HTTP/1.1") //a verziju imamo od malopre
		{
			string msg = "HTTP version not supported!";
			SendError(clientSock, 505, msg); //greska servera, jer je klijent uneo ispravnu verziju dakle ispravan HTTP zahtev a server ne podrzava tu verziju(server je kriv)
			closesocket(clientSock);
			continue;
		}

		//4) Proveri da li je metoda GET
		if (method != "GET")
		{
			string msg = "Bad request";
			SendError(clientSock, 400, msg);
			closesocket(clientSock);
			continue;
		}

		//5) Mapiraj path na file + validacija
		string filePath;
		if (!MapUrlToFile(path, filePath))
		{
			string msg = "Bad request";
			SendError(clientSock, 400, msg);
			closesocket(clientSock);
			continue;
		}

		//6) Proveri da li filePath slike postoji
		if (!FileExists(filePath))
		{
			string msg = "Not Found";
			SendError(clientSock, 404, msg);
			closesocket(clientSock);
			continue;
		}

		//7) Ucitaj fajl i posalji 200 + png
		vector<char> bytes; //deo body-ja su bajtovi, pa ne moze string nego se koristi vector<char>
		if (!ReadFileBytes(filePath, bytes))
		{
			string msg = "Not Found!";
			SendError(clientSock, 404, msg);
			closesocket(clientSock);
			continue;
		}

		sendpng(clientSock, bytes); //ako je sve uredu posalji sliku tj header+bytes

		closesocket(clientSock);

	}

	closesocket(listensock);
	WSACleanup();

}
