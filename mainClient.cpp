#include <iostream>
using namespace std;
#include <string>
#include <winsock.h>
#include <sstream>
#include <vector>
#include <fstream>
#pragma comment(lib, "wsock32.lib")

#define BUF_SIZE 1024

void ExitWithError(const string& message)
{
	cout << message << "Code error: " << WSAGetLastError() << endl;
	WSACleanup();
	exit(1);
}

void SendRequest(SOCKET s, const string& path)
{
	ostringstream os;

	os << "GET " << path << " HTTP/1.1\r\n"
		<< "Connection: close\r\n"
		<< "User - agent: Mozilla / 4.0\r\n"
		<< "Accept: text/html, image/gif, image/jpeg\r\n"
		<< "Accept-language:fr\r\n\r\n";

	string req = os.str();
	//moze send() umesto sendAll() jer se salje samo zahtev. Kod servera se slao ceo png fajl koji ima mnogo bajtova pa je moralo sendAll()
	send(s, req.c_str(), (int)req.size(), 0);
}

bool RecvHeader(SOCKET s, string& header, string &leftover)
{
	header.clear();
	leftover.clear();
	char buff[512];
	string acc; //ovo je akumulator u koji skupljamo bajtove dok ne dodjemo do kraja headera. Znaci svi bajtovi iz head + neki iz body ce biti ovde

	while (true)
	{
		int n = recv(s, buff, sizeof(buff), 0);

		if (n <= 0)
			return false;

		acc.append(buff, buff + n);

		size_t p = acc.find("\r\n\r\n");

		if (p != string::npos) //nadjen je kraj headera. Ako ne, nastavi da citas dok ne nadjes
		{
			size_t bodyStart = p + 4; //prvi bajt nakon head

			header = acc.substr(0, bodyStart);

			if (acc.size() > header.size()) //acc je uzeo deo od body. Smesti taj deo u leftover.
			{
				leftover = acc.substr(bodyStart);
			}

			return true;
		}

		if (header.size() > 8192)
			return false;
	}
}

bool GetStatusCode(const string& header, int& code)
{
	size_t e = header.find("\r\n");

	if (e == string::npos)
		return false;

	istringstream is(header.substr(0, e)); //izdvojena prva recenica

	string version;
	if (!(is >> version >> code)) //radi samo ako server vraca http odgovor u obliku: Verzija + kod + ostalo(error message) 
		return false;

	return true;
}

void ReadError(SOCKET s)
{
	char buff[1024];
	string body;
	int n;

	if ((n = recv(s, buff, sizeof(buff), 0)) > 0)
	{
		body.append(buff, buff + n);
	}

	if (!body.empty())
	{
		cout << body << endl;
	}
}

bool ReadContentLength(const string& header, int& contentLength)
{
	size_t pos = header.find("Content-Length:"); //vraca indeks prve pozicije na kojoj je Content-length

	if (pos == string::npos)
		return false; //nije nadjen Content length

	pos += strlen("Content-Length:"); //pomeri se da pos pokazuje na indeks odmah nakon dvotacke

	contentLength = atoi(header.c_str() + pos); //cita sve brojke dok ne dodje na nesto sto nije brojka. A krece od header(pocetak)+pos znaci od odma nakon :

	return true;
}

bool RecvBody(SOCKET s, int contentLength, const string& leftover, vector<char>& body)
{
	//iz prethodnog recv je dobijen head, sad se prima dalje body
	body.clear();
	body.reserve((size_t)contentLength); //body ima onoliko bajtova koliko je contentLength

	//ubaci prvo vec primljeni deo iz leftover u body
	int already = leftover.size();

	if (already > 0)
	{
		body.insert(body.end(), leftover.begin(), leftover.end()); 
	}

	if (body.size() > contentLength) //ako vec ima vise bajtova nego contentLength - skrati da bude contentLength (onda se i ne ulazi dalje u while)
	{
		body.resize((size_t)contentLength);
	}

	//sad citaj ostatak body-ja
	char buff[512];
	int total = body.size();
	while (total < contentLength)
	{
		int n = recv(s, buff, min(sizeof(buff), contentLength - total), 0);

		if (n <= 0)
			return false;

		body.insert(body.end(), buff, buff + n);
		total += n;
	}

	return true;
}

int main()
{
	WSAData wsd;
	SOCKET clientSock; 
	int serverPort;
	string addrServer;

	if (WSAStartup(0x0202, &wsd) != 0)
		ExitWithError("Startup failed! ");

	if ((clientSock = socket(AF_INET, SOCK_STREAM, 0)) == INVALID_SOCKET)
		ExitWithError("Socket not created! ");

	sockaddr_in server;
	server.sin_family = AF_INET;

	cout << "Unesi adresu servera: ";
	getline(cin, addrServer);
	server.sin_addr.s_addr = inet_addr(addrServer.c_str());

	cout << "Unesi port servera: ";
	cin >> serverPort;
	cin.ignore();

	cout << "Unesi putanju slike: ";
	string path;
	getline(cin, path);
	
	if (path.empty())
		ExitWithError("Uneli ste praznu putanju slike! ");

	if (path[0] != '/')
		path = '/' + path;

	server.sin_port = htons(serverPort);

	//connect() pokusava da ostvari konekciju sa serverom:
	if (connect(clientSock, (struct sockaddr*)&server, sizeof(server)) == SOCKET_ERROR)
		ExitWithError("connect() failed! ");

	//1) Posalji HTTP zahtev
	SendRequest(clientSock, path);

	//2) Primi header
	string header;
	string leftover; //uvodi se zato sto recv() moze da procita i dodatne bitove koji nisu head nego su pocetak od body. Ti bitovi isto moraju da se sacuvaju
					 //f-ja ce da procita header i ostatak(body) ce da stavi u leftover
	if (!RecvHeader(clientSock, header, leftover))
	{
		cout << "Klijent nije uspeo da dobije header. \n";
		closesocket(clientSock);
		WSACleanup();
		return 1;
	}

	//3) Procitaj status code
	int code = 0;
	if (!GetStatusCode(header, code))
	{
		cout << "Greska. Nije dobijen status kod! \n";
		closesocket(clientSock);
		WSACleanup();
		return 1;
	}

	cout << "Status: " << code << endl;

	if (code == 304)
	{
		cout << "Nije modifikovano posle 10.12.2020.\n";
		closesocket(clientSock);
		WSACleanup();
		return 0;
	}

	if (code != 200) //server je vratio gresku. Procitaj celu gresku
	{
		ReadError(clientSock);
		closesocket(clientSock);
		WSACleanup();
		return 0;
	}

	//4) Procitaj Content-length - treba mi da bih dobio body. A u body-ju je slika(svi bitovi) ako je kod 200 ili je greska
	int contentLength; //predstavlja broj bajtova koji je u body-ju
	if (!ReadContentLength(header,  contentLength))
	{
		cout << "Greska. Nije procitan Content-Length.\n";
		closesocket(clientSock);
		WSACleanup();
		return 0;
	}

	//5) Procitaj body - bajtove slike
	vector<char> body;
	if (!RecvBody(clientSock,contentLength, leftover, body))
	{
		cout << "Greska. Nije procitan body.\n";
		closesocket(clientSock);
		WSACleanup();
		return 0;
	}

	//6) Svi bajtovi png slike su u body. Sad snimi tu sliku u fajl.
	ofstream f("download.png", ios::binary);
	if (!f)
	{
		cout << "Greska. Neuspesno otvaranje fajla download.png" << endl;
		return 1;
	}

	f.write(body.data(), (streamsize)body.size());
	f.close();

	cout << "Sacuvano u download.png (" << body.size() << " bajtova)\n";

	closesocket(clientSock);
	WSACleanup();
	return 0;
}
