#include "Rotor.h"
#include "GmpUtil.h"
#include "Base58.h"
#include "hash/sha256.h"
#include "hash/keccak160.h"
#include "IntGroup.h"
#include "Timer.h"
#include "hash/ripemd160.h"
#include <cstring>
#include <cmath>
#include <algorithm>
#include <iostream>
#include <cassert>
#ifndef WIN64
#include <pthread.h>
#endif

using namespace std;

Point Gn[CPU_GRP_SIZE / 2];
Point _2Gn;

// ----------------------------------------------------------------------------

Rotor::Rotor(const std::string& inputFile, int compMode, int searchMode, int coinType, bool useGpu,
	const std::string& outputFile, bool useSSE, uint32_t maxFound, uint64_t rKey,
	const std::string& rangeStart, const std::string& rangeEnd, bool& should_exit)
{
	this->compMode = compMode;
	this->useGpu = useGpu;
	this->outputFile = outputFile;
	this->useSSE = useSSE;
	this->nbGPUThread = 0;
	this->inputFile = inputFile;
	this->maxFound = maxFound;
	this->rKey = rKey;
	this->searchMode = searchMode;
	this->coinType = coinType;
	this->rangeStart.SetBase16(rangeStart.c_str());
	this->rangeEnd.SetBase16(rangeEnd.c_str());
	this->rangeDiff2.Set(&this->rangeEnd);
	this->rangeDiff2.Sub(&this->rangeStart);
	this->lastrKey = 0;

	secp = new Secp256K1();
	secp->Init();

	// load file
	FILE* wfd;
	uint64_t N = 0;

	wfd = fopen(this->inputFile.c_str(), "rb");
	if (!wfd) {
		printf("  %s can not open\n", this->inputFile.c_str());
		exit(1);
	}

#ifdef WIN64
	_fseeki64(wfd, 0, SEEK_END);
	N = _ftelli64(wfd);
#else
	fseek(wfd, 0, SEEK_END);
	N = ftell(wfd);
#endif

	int K_LENGTH = 20;
	if (this->searchMode == (int)SEARCH_MODE_MX)
		K_LENGTH = 32;

	N = N / K_LENGTH;
	rewind(wfd);

	DATA = (uint8_t*)malloc(N * K_LENGTH);
	memset(DATA, 0, N * K_LENGTH);

	uint8_t* buf = (uint8_t*)malloc(K_LENGTH);;

	bloom = new Bloom(2 * N, 0.000001);

	uint64_t percent = (N - 1) / 100;
	uint64_t i = 0;
	printf("\n");
	while (i < N && !should_exit) {
		memset(buf, 0, K_LENGTH);
		memset(DATA + (i * K_LENGTH), 0, K_LENGTH);
		if (fread(buf, 1, K_LENGTH, wfd) == K_LENGTH) {
			bloom->add(buf, K_LENGTH);
			memcpy(DATA + (i * K_LENGTH), buf, K_LENGTH);
			if ((percent != 0) && i % percent == 0) {
				printf("\r  Loading      : %llu %%", (i / percent));
				fflush(stdout);
			}
		}
		i++;
	}
	fclose(wfd);
	free(buf);

	if (should_exit) {
		delete secp;
		delete bloom;
		if (DATA)
			free(DATA);
		exit(0);
	}

	BLOOM_N = bloom->get_bytes();
	TOTAL_COUNT = N;
	targetCounter = i;
	if (coinType == COIN_BTC) {
		if (searchMode == (int)SEARCH_MODE_MA)
			printf("\n  Loaded       : %s Bitcoin addresses\n", formatThousands(i).c_str());
		else if (searchMode == (int)SEARCH_MODE_MX)
			printf("\n  Loaded       : %s Bitcoin xpoints\n", formatThousands(i).c_str());
	}
	else {
		printf("\n  Loaded       : %s Ethereum addresses\n", formatThousands(i).c_str());
	}

	printf("\n");

	bloom->print();
	printf("\n");

	InitGenratorTable();

}

// ----------------------------------------------------------------------------

Rotor::Rotor(const std::vector<unsigned char>& hashORxpoint, int compMode, int searchMode, int coinType,
	bool useGpu, const std::string& outputFile, bool useSSE, uint32_t maxFound, uint64_t rKey,
	const std::string& rangeStart, const std::string& rangeEnd, bool& should_exit)
{
	this->compMode = compMode;
	this->useGpu = useGpu;
	this->outputFile = outputFile;
	this->useSSE = useSSE;
	this->nbGPUThread = 0;
	this->maxFound = maxFound;
	this->rKey = rKey;
	this->searchMode = searchMode;
	this->coinType = coinType;
	this->rangeStart.SetBase16(rangeStart.c_str());
	this->rangeEnd.SetBase16(rangeEnd.c_str());
	this->rangeDiff2.Set(&this->rangeEnd);
	this->rangeDiff2.Sub(&this->rangeStart);
	this->targetCounter = 1;

	secp = new Secp256K1();
	secp->Init();

	if (this->searchMode == (int)SEARCH_MODE_SA) {
		assert(hashORxpoint.size() == 20);
		for (size_t i = 0; i < hashORxpoint.size(); i++) {
			((uint8_t*)hash160Keccak)[i] = hashORxpoint.at(i);
		}
	}
	else if (this->searchMode == (int)SEARCH_MODE_SX) {
		assert(hashORxpoint.size() == 32);
		for (size_t i = 0; i < hashORxpoint.size(); i++) {
			((uint8_t*)xpoint)[i] = hashORxpoint.at(i);
		}
	}
	printf("\n");

	InitGenratorTable();
}

// ----------------------------------------------------------------------------

void Rotor::InitGenratorTable()
{
	// Compute Generator table G[n] = (n+1)*G
	Point g = secp->G;
	Gn[0] = g;
	g = secp->DoubleDirect(g);
	Gn[1] = g;
	for (int i = 2; i < CPU_GRP_SIZE / 2; i++) {
		g = secp->AddDirect(g, secp->G);
		Gn[i] = g;
	}
	// _2Gn = CPU_GRP_SIZE*G
	_2Gn = secp->DoubleDirect(Gn[CPU_GRP_SIZE / 2 - 1]);

	char* ctimeBuff;
	time_t now = time(NULL);
	ctimeBuff = ctime(&now);
	printf("  Start Time   : %s", ctimeBuff);

	if (rKey == 0) {
		printf("  Global start : %s (%d bit)\n", this->rangeStart.GetBase16().c_str(), this->rangeStart.GetBitLength());
		printf("  Global end   : %s (%d bit)\n", this->rangeEnd.GetBase16().c_str(), this->rangeEnd.GetBitLength());
		printf("  Global range : %s (%d bit)\n", this->rangeDiff2.GetBase16().c_str(), this->rangeDiff2.GetBitLength());
	}
}

// ----------------------------------------------------------------------------

Rotor::~Rotor()
{
	delete secp;
	if (searchMode == (int)SEARCH_MODE_MA || searchMode == (int)SEARCH_MODE_MX)
		delete bloom;
	if (DATA)
		free(DATA);
}

// ----------------------------------------------------------------------------

double log1(double x)
{
	// Use taylor series to approximate log(1-x)
	return -x - (x * x) / 2.0 - (x * x * x) / 3.0 - (x * x * x * x) / 4.0;
}

void Rotor::output(std::string addr, std::string pAddr, std::string pAddrHex, std::string pubKey)
{

#ifdef WIN64
	WaitForSingleObject(ghMutex, INFINITE);
#else
	pthread_mutex_lock(&ghMutex);
#endif

	FILE* f = stdout;
	bool needToClose = false;

	if (outputFile.length() > 0) {
		f = fopen(outputFile.c_str(), "a");
		if (f == NULL) {
			printf("  Cannot open %s for writing\n", outputFile.c_str());
			f = stdout;
		}
		else {
			needToClose = true;
		}
	}

	if (!needToClose)
		printf("\n");
	#ifdef WIN64
	HANDLE hConsole = GetStdHandle(STD_OUTPUT_HANDLE);
	SetConsoleTextAttribute(hConsole, FOREGROUND_BLUE | FOREGROUND_GREEN | FOREGROUND_INTENSITY);
	#endif
	fprintf(f, "PubAddress: %s\n", addr.c_str());
	fprintf(stdout, "\n  =================================================================================\n");
	fprintf(stdout, "  PubAddress: %s\n", addr.c_str());

	if (coinType == COIN_BTC) {
		fprintf(f, "Priv (WIF): p2pkh:%s\n", pAddr.c_str());
		fprintf(stdout, "  Priv (WIF): p2pkh:%s\n", pAddr.c_str());
	}

	fprintf(f, "Priv (HEX): %s\n", pAddrHex.c_str());
	fprintf(stdout, "  Priv (HEX): %s\n", pAddrHex.c_str());

	fprintf(f, "PubK (HEX): %s\n", pubKey.c_str());
	fprintf(stdout, "  PubK (HEX): %s\n", pubKey.c_str());

	fprintf(f, "=================================================================================\n");
	fprintf(stdout, "  =================================================================================\n");

	if (needToClose)
		fclose(f);

#ifdef WIN64
	ReleaseMutex(ghMutex);
#else
	pthread_mutex_unlock(&ghMutex);
#endif

}

// ----------------------------------------------------------------------------

bool Rotor::checkPrivKey(std::string addr, Int& key, int32_t incr, bool mode)
{
	Int k(&key), k2(&key);
	k.Add((uint64_t)incr);
	k2.Add((uint64_t)incr);
	// Check addresses
	Point p = secp->ComputePublicKey(&k);
	std::string px = p.x.GetBase16();
	std::string chkAddr = secp->GetAddress(mode, p);
	if (chkAddr != addr) {
		//Key may be the opposite one (negative zero or compressed key)
		k.Neg();
		k.Add(&secp->order);
		p = secp->ComputePublicKey(&k);
		std::string chkAddr = secp->GetAddress(mode, p);
		if (chkAddr != addr) {
			printf("\n=================================================================================\n");
			printf("  Warning, wrong private key generated !\n");
			printf("  PivK : %s\n", k2.GetBase16().c_str());
			printf("  Addr : %s\n", addr.c_str());
			printf("  PubX : %s\n", px.c_str());
			printf("  PivK : %s\n", k.GetBase16().c_str());
			printf("  Check: %s\n", chkAddr.c_str());
			printf("  PubX : %s\n", p.x.GetBase16().c_str());
			printf("=================================================================================\n");
			return false;
		}
	}
	output(addr, secp->GetPrivAddress(mode, k), k.GetBase16(), secp->GetPublicKeyHex(mode, p));
	return true;
}

bool Rotor::checkPrivKeyETH(std::string addr, Int& key, int32_t incr)
{
	Int k(&key), k2(&key);
	k.Add((uint64_t)incr);
	k2.Add((uint64_t)incr);
	// Check addresses
	Point p = secp->ComputePublicKey(&k);
	std::string px = p.x.GetBase16();
	std::string chkAddr = secp->GetAddressETH(p);
	if (chkAddr != addr) {
		//Key may be the opposite one (negative zero or compressed key)
		k.Neg();
		k.Add(&secp->order);
		p = secp->ComputePublicKey(&k);
		std::string chkAddr = secp->GetAddressETH(p);
		if (chkAddr != addr) {
			printf("\n=================================================================================\n");
			printf("  Warning, wrong private key generated !\n");
			printf("  PivK :%s\n", k2.GetBase16().c_str());
			printf("  Addr :%s\n", addr.c_str());
			printf("  PubX :%s\n", px.c_str());
			printf("  PivK :%s\n", k.GetBase16().c_str());
			printf("  Check:%s\n", chkAddr.c_str());
			printf("  PubX :%s\n", p.x.GetBase16().c_str());
			printf("=================================================================================\n");
			return false;
		}
	}
	output(addr, k.GetBase16()/*secp->GetPrivAddressETH(k)*/, k.GetBase16(), secp->GetPublicKeyHexETH(p));
	return true;
}

bool Rotor::checkPrivKeyX(Int& key, int32_t incr, bool mode)
{
	Int k(&key);
	k.Add((uint64_t)incr);
	Point p = secp->ComputePublicKey(&k);
	std::string addr = secp->GetAddress(mode, p);
	output(addr, secp->GetPrivAddress(mode, k), k.GetBase16(), secp->GetPublicKeyHex(mode, p));
	return true;
}

// ----------------------------------------------------------------------------

#ifdef WIN64
DWORD WINAPI _FindKeyCPU(LPVOID lpParam)
{
#else
void* _FindKeyCPU(void* lpParam)
{
#endif
	TH_PARAM* p = (TH_PARAM*)lpParam;
	p->obj->FindKeyCPU(p);
	return 0;
}

#ifdef WIN64
DWORD WINAPI _FindKeyGPU(LPVOID lpParam)
{
#else
void* _FindKeyGPU(void* lpParam)
{
#endif
	TH_PARAM* p = (TH_PARAM*)lpParam;
	p->obj->FindKeyGPU(p);
	return 0;
}

// ----------------------------------------------------------------------------

void Rotor::checkMultiAddresses(bool compressed, Int key, int i, Point p1)
{
	unsigned char h0[20];

	// Point
	secp->GetHash160(compressed, p1, h0);
	if (CheckBloomBinary(h0, 20) > 0) {
		std::string addr = secp->GetAddress(compressed, h0);
		if (checkPrivKey(addr, key, i, compressed)) {
			nbFoundKey++;
		}
	}
}

// ----------------------------------------------------------------------------

void Rotor::checkMultiAddressesETH(Int key, int i, Point p1)
{
	unsigned char h0[20];

	// Point
	secp->GetHashETH(p1, h0);
	if (CheckBloomBinary(h0, 20) > 0) {
		std::string addr = secp->GetAddressETH(h0);
		if (checkPrivKeyETH(addr, key, i)) {
			nbFoundKey++;
		}
	}
}

// ----------------------------------------------------------------------------

void Rotor::checkSingleAddress(bool compressed, Int key, int i, Point p1)
{
	unsigned char h0[20];

	// Point
	secp->GetHash160(compressed, p1, h0);
	if (MatchHash((uint32_t*)h0)) {
		std::string addr = secp->GetAddress(compressed, h0);
		if (checkPrivKey(addr, key, i, compressed)) {
			nbFoundKey++;
		}
	}
}

// ----------------------------------------------------------------------------

void Rotor::checkSingleAddressETH(Int key, int i, Point p1)
{
	unsigned char h0[20];

	// Point
	secp->GetHashETH(p1, h0);
	if (MatchHash((uint32_t*)h0)) {
		std::string addr = secp->GetAddressETH(h0);
		if (checkPrivKeyETH(addr, key, i)) {
			nbFoundKey++;
		}
	}
}

// ----------------------------------------------------------------------------

void Rotor::checkMultiXPoints(bool compressed, Int key, int i, Point p1)
{
	unsigned char h0[32];

	// Point
	secp->GetXBytes(compressed, p1, h0);
	if (CheckBloomBinary(h0, 32) > 0) {
		if (checkPrivKeyX(key, i, compressed)) {
			nbFoundKey++;
		}
	}
}

// ----------------------------------------------------------------------------

void Rotor::checkSingleXPoint(bool compressed, Int key, int i, Point p1)
{
	unsigned char h0[32];

	// Point
	secp->GetXBytes(compressed, p1, h0);
	if (MatchXPoint((uint32_t*)h0)) {
		if (checkPrivKeyX(key, i, compressed)) {
			nbFoundKey++;
		}
	}
}

// ----------------------------------------------------------------------------

void Rotor::checkMultiAddressesSSE(bool compressed, Int key, int i, Point p1, Point p2, Point p3, Point p4)
{
	unsigned char h0[20];
	unsigned char h1[20];
	unsigned char h2[20];
	unsigned char h3[20];

	// Point -------------------------------------------------------------------------
	secp->GetHash160(compressed, p1, p2, p3, p4, h0, h1, h2, h3);
	if (CheckBloomBinary(h0, 20) > 0) {
		std::string addr = secp->GetAddress(compressed, h0);
		if (checkPrivKey(addr, key, i + 0, compressed)) {
			nbFoundKey++;
		}
	}
	if (CheckBloomBinary(h1, 20) > 0) {
		std::string addr = secp->GetAddress(compressed, h1);
		if (checkPrivKey(addr, key, i + 1, compressed)) {
			nbFoundKey++;
		}
	}
	if (CheckBloomBinary(h2, 20) > 0) {
		std::string addr = secp->GetAddress(compressed, h2);
		if (checkPrivKey(addr, key, i + 2, compressed)) {
			nbFoundKey++;
		}
	}
	if (CheckBloomBinary(h3, 20) > 0) {
		std::string addr = secp->GetAddress(compressed, h3);
		if (checkPrivKey(addr, key, i + 3, compressed)) {
			nbFoundKey++;
		}
	}

}

// ----------------------------------------------------------------------------

void Rotor::checkSingleAddressesSSE(bool compressed, Int key, int i, Point p1, Point p2, Point p3, Point p4)
{
	unsigned char h0[20];
	unsigned char h1[20];
	unsigned char h2[20];
	unsigned char h3[20];

	// Point -------------------------------------------------------------------------
	secp->GetHash160(compressed, p1, p2, p3, p4, h0, h1, h2, h3);
	if (MatchHash((uint32_t*)h0)) {
		std::string addr = secp->GetAddress(compressed, h0);
		if (checkPrivKey(addr, key, i + 0, compressed)) {
			nbFoundKey++;
		}
	}
	if (MatchHash((uint32_t*)h1)) {
		std::string addr = secp->GetAddress(compressed, h1);
		if (checkPrivKey(addr, key, i + 1, compressed)) {
			nbFoundKey++;
		}
	}
	if (MatchHash((uint32_t*)h2)) {
		std::string addr = secp->GetAddress(compressed, h2);
		if (checkPrivKey(addr, key, i + 2, compressed)) {
			nbFoundKey++;
		}
	}
	if (MatchHash((uint32_t*)h3)) {
		std::string addr = secp->GetAddress(compressed, h3);
		if (checkPrivKey(addr, key, i + 3, compressed)) {
			nbFoundKey++;
		}
	}

}

// ----------------------------------------------------------------------------

void Rotor::getCPUStartingKey(Int & tRangeStart, Int & tRangeEnd, Int & key, Point & startP)
{
	if (rKey <= 0) {
		// Sequential mode: start from the beginning of the thread's range
		key.Set(&tRangeStart);
		printf("  CPU Core     : Start from range %s -> \n\n", key.GetBase16().c_str());
	}
	else {
		// Hybrid mode: Generate random key within this thread's specific range
		Int threadRangeDiff;
		threadRangeDiff.Set(&tRangeEnd);
		threadRangeDiff.Sub(&tRangeStart);
		
		key.Rand(&threadRangeDiff);  // Random number from 0 to (tRangeEnd-tRangeStart)
		key.Add(&tRangeStart);       // Shift to this thread's range: tRangeStart to tRangeEnd
	}
	rhex = key;
	Int km(&key);
	km.Add((uint64_t)CPU_GRP_SIZE / 2);
	startP = secp->ComputePublicKey(&km);
}

// ----------------------------------------------------------------------------

void Rotor::FindKeyCPU(TH_PARAM * ph)
{

	// Global init
	int thId = ph->threadId;
	
	Int tRangeStart = ph->rangeStart;
	Int tRangeEnd = ph->rangeEnd;
	counters[thId] = 0;
	
	if (rKey > 0) {
		if (rKeyCount2 == 0) {
			if (thId == 0) {
				printf("  Base Key     : Randomly changes %d Private keys every %llu.000.000.000 on the counter\n\n", nbCPUThread, rKey);
			}
		}
	}
	// CPU Thread
	IntGroup* grp = new IntGroup(CPU_GRP_SIZE / 2 + 1);

	// Group Init
	Int key;// = new Int();
	Point startP;// = new Point();
	getCPUStartingKey(tRangeStart, tRangeEnd, key, startP);

	Int* dx = new Int[CPU_GRP_SIZE / 2 + 1];
	Point* pts = new Point[CPU_GRP_SIZE];

	Int* dy = new Int();
	Int* dyn = new Int();
	Int* _s = new Int();
	Int* _p = new Int();
	Point* pp = new Point();
	Point* pn = new Point();
	grp->Set(dx);

	ph->hasStarted = true;
	ph->rKeyRequest = false;

	while (!endOfSearch) {

		if (ph->rKeyRequest) {
			getCPUStartingKey(tRangeStart, tRangeEnd, key, startP);
			ph->rKeyRequest = false;
		}

		// Fill group
		int i;
		int hLength = (CPU_GRP_SIZE / 2 - 1);

		for (i = 0; i < hLength; i++) {
			dx[i].ModSub(&Gn[i].x, &startP.x);
		}
		dx[i].ModSub(&Gn[i].x, &startP.x);  // For the first point
		dx[i + 1].ModSub(&_2Gn.x, &startP.x); // For the next center point

		// Grouped ModInv
		grp->ModInv();

		// We use the fact that P + i*G and P - i*G has the same deltax, so the same inverse
		// We compute key in the positive and negative way from the center of the group

		// center point
		pts[CPU_GRP_SIZE / 2] = startP;

		for (i = 0; i < hLength && !endOfSearch; i++) {

			*pp = startP;
			*pn = startP;

			// P = startP + i*G
			dy->ModSub(&Gn[i].y, &pp->y);

			_s->ModMulK1(dy, &dx[i]);       // s = (p2.y-p1.y)*inverse(p2.x-p1.x);
			_p->ModSquareK1(_s);            // _p = pow2(s)

			pp->x.ModNeg();
			pp->x.ModAdd(_p);
			pp->x.ModSub(&Gn[i].x);           // rx = pow2(s) - p1.x - p2.x;

			pp->y.ModSub(&Gn[i].x, &pp->x);
			pp->y.ModMulK1(_s);
			pp->y.ModSub(&Gn[i].y);           // ry = - p2.y - s*(ret.x-p2.x);

			// P = startP - i*G  , if (x,y) = i*G then (x,-y) = -i*G
			dyn->Set(&Gn[i].y);
			dyn->ModNeg();
			dyn->ModSub(&pn->y);

			_s->ModMulK1(dyn, &dx[i]);      // s = (p2.y-p1.y)*inverse(p2.x-p1.x);
			_p->ModSquareK1(_s);            // _p = pow2(s)

			pn->x.ModNeg();
			pn->x.ModAdd(_p);
			pn->x.ModSub(&Gn[i].x);          // rx = pow2(s) - p1.x - p2.x;

			pn->y.ModSub(&Gn[i].x, &pn->x);
			pn->y.ModMulK1(_s);
			pn->y.ModAdd(&Gn[i].y);          // ry = - p2.y - s*(ret.x-p2.x);

			pts[CPU_GRP_SIZE / 2 + (i + 1)] = *pp;
			pts[CPU_GRP_SIZE / 2 - (i + 1)] = *pn;

		}

		// First point (startP - (GRP_SZIE/2)*G)
		*pn = startP;
		dyn->Set(&Gn[i].y);
		dyn->ModNeg();
		dyn->ModSub(&pn->y);

		_s->ModMulK1(dyn, &dx[i]);
		_p->ModSquareK1(_s);

		pn->x.ModNeg();
		pn->x.ModAdd(_p);
		pn->x.ModSub(&Gn[i].x);

		pn->y.ModSub(&Gn[i].x, &pn->x);
		pn->y.ModMulK1(_s);
		pn->y.ModAdd(&Gn[i].y);

		pts[0] = *pn;

		// Next start point (startP + GRP_SIZE*G)
		*pp = startP;
		dy->ModSub(&_2Gn.y, &pp->y);

		_s->ModMulK1(dy, &dx[i + 1]);
		_p->ModSquareK1(_s);

		pp->x.ModNeg();
		pp->x.ModAdd(_p);
		pp->x.ModSub(&_2Gn.x);

		pp->y.ModSub(&_2Gn.x, &pp->x);
		pp->y.ModMulK1(_s);
		pp->y.ModSub(&_2Gn.y);
		startP = *pp;

		// Check addresses
		if (useSSE) {
			for (int i = 0; i < CPU_GRP_SIZE && !endOfSearch; i += 4) {
				switch (compMode) {
				case SEARCH_COMPRESSED:
					if (searchMode == (int)SEARCH_MODE_MA) {
						checkMultiAddressesSSE(true, key, i, pts[i], pts[i + 1], pts[i + 2], pts[i + 3]);
					}
					else if (searchMode == (int)SEARCH_MODE_SA) {
						checkSingleAddressesSSE(true, key, i, pts[i], pts[i + 1], pts[i + 2], pts[i + 3]);
					}
					break;
				case SEARCH_UNCOMPRESSED:
					if (searchMode == (int)SEARCH_MODE_MA) {
						checkMultiAddressesSSE(false, key, i, pts[i], pts[i + 1], pts[i + 2], pts[i + 3]);
					}
					else if (searchMode == (int)SEARCH_MODE_SA) {
						checkSingleAddressesSSE(false, key, i, pts[i], pts[i + 1], pts[i + 2], pts[i + 3]);
					}
					break;
				case SEARCH_BOTH:
					if (searchMode == (int)SEARCH_MODE_MA) {
						checkMultiAddressesSSE(true, key, i, pts[i], pts[i + 1], pts[i + 2], pts[i + 3]);
						checkMultiAddressesSSE(false, key, i, pts[i], pts[i + 1], pts[i + 2], pts[i + 3]);
					}
					else if (searchMode == (int)SEARCH_MODE_SA) {
						checkSingleAddressesSSE(true, key, i, pts[i], pts[i + 1], pts[i + 2], pts[i + 3]);
						checkSingleAddressesSSE(false, key, i, pts[i], pts[i + 1], pts[i + 2], pts[i + 3]);
					}
					break;
				}
			}
		}
		else {
			if (coinType == COIN_BTC) {
				for (int i = 0; i < CPU_GRP_SIZE && !endOfSearch; i++) {
					switch (compMode) {
					case SEARCH_COMPRESSED:
						switch (searchMode) {
						case (int)SEARCH_MODE_MA:
							checkMultiAddresses(true, key, i, pts[i]);
							break;
						case (int)SEARCH_MODE_SA:
							checkSingleAddress(true, key, i, pts[i]);
							break;
						case (int)SEARCH_MODE_MX:
							checkMultiXPoints(true, key, i, pts[i]);
							break;
						case (int)SEARCH_MODE_SX:
							checkSingleXPoint(true, key, i, pts[i]);
							break;
						default:
							break;
						}
						break;
					case SEARCH_UNCOMPRESSED:
						switch (searchMode) {
						case (int)SEARCH_MODE_MA:
							checkMultiAddresses(false, key, i, pts[i]);
							break;
						case (int)SEARCH_MODE_SA:
							checkSingleAddress(false, key, i, pts[i]);
							break;
						case (int)SEARCH_MODE_MX:
							checkMultiXPoints(false, key, i, pts[i]);
							break;
						case (int)SEARCH_MODE_SX:
							checkSingleXPoint(false, key, i, pts[i]);
							break;
						default:
							break;
						}
						break;
					case SEARCH_BOTH:
						switch (searchMode) {
						case (int)SEARCH_MODE_MA:
							checkMultiAddresses(true, key, i, pts[i]);
							checkMultiAddresses(false, key, i, pts[i]);
							break;
						case (int)SEARCH_MODE_SA:
							checkSingleAddress(true, key, i, pts[i]);
							checkSingleAddress(false, key, i, pts[i]);
							break;
						case (int)SEARCH_MODE_MX:
							checkMultiXPoints(true, key, i, pts[i]);
							checkMultiXPoints(false, key, i, pts[i]);
							break;
						case (int)SEARCH_MODE_SX:
							checkSingleXPoint(true, key, i, pts[i]);
							checkSingleXPoint(false, key, i, pts[i]);
							break;
						default:
							break;
						}
						break;
					}
				}
			}
			else {
				for (int i = 0; i < CPU_GRP_SIZE && !endOfSearch; i++) {
					switch (searchMode) {
					case (int)SEARCH_MODE_MA:
						checkMultiAddressesETH(key, i, pts[i]);
						break;
					case (int)SEARCH_MODE_SA:
						checkSingleAddressETH(key, i, pts[i]);
						break;
					default:
						break;
					}
				}
			}
		}
		key.Add((uint64_t)CPU_GRP_SIZE);
		counters[thId] += CPU_GRP_SIZE; // Point
	}
	ph->isRunning = false;

	delete grp;
	delete[] dx;
	delete[] pts;

	delete dy;
	delete dyn;
	delete _s;
	delete _p;
	delete pp;
	delete pn;
}

// ----------------------------------------------------------------------------

void Rotor::getGPUStartingKeys(Int & tRangeStart, Int & tRangeEnd, int groupSize, int nbThread, Int * keys, Point * p)
{
	if (rKey > 0) {
		if (rKeyCount2 == 0) {
			printf("  Base Key     : Randomly changes %d start Private keys every %llu.000.000.000 on the counter\n\n", nbThread, rKey);
		}
		
		// Calculate the range for each GPU thread
		Int totalGpuRange;
		totalGpuRange.Set(&tRangeEnd);
		totalGpuRange.Sub(&tRangeStart);
		
		Int threadRange;
		Int nbThreadsInt;
		nbThreadsInt.SetInt32(nbThread);
		threadRange.Set(&totalGpuRange);
		threadRange.Div(&nbThreadsInt);
		
		for (int i = 0; i < nbThread; i++) {
			// Calculate this thread's range
			Int threadStart, threadEnd;
			threadStart.Set(&tRangeStart);
			Int offset;
			offset.Mult(&threadRange, (uint64_t)i);
			threadStart.Add(&offset);
			
			threadEnd.Set(&threadStart);
			threadEnd.Add(&threadRange);
			
			// Generate random key within this thread's specific range
			Int threadRangeDiff;
			threadRangeDiff.Set(&threadEnd);
			threadRangeDiff.Sub(&threadStart);
			
			keys[i].Rand(&threadRangeDiff);
			keys[i].Add(&threadStart);
			rhex = keys[i];
			
			Int k(keys + i);
			k.Add((uint64_t)(groupSize / 2));
			p[i] = secp->ComputePublicKey(&k);
		}
	}
	else {

		Int tRangeDiff(tRangeEnd);
		Int tRangeStart2(tRangeStart);
		Int tRangeEnd2(tRangeStart);

		Int tThreads;
		tThreads.SetInt32(nbThread);
		tRangeDiff.Set(&tRangeEnd);
		tRangeDiff.Sub(&tRangeStart);
		razn = tRangeDiff;
		tRangeDiff.Div(&tThreads);

		int rangeShowThreasold = 3;
		int rangeShowCounter = 0;
		printf("  Divide the range %s into %d threads for fast parallel search \n", razn.GetBase16().c_str(), nbThread);
		for (int i = 0; i < nbThread +1; i++) {

			tRangeEnd2.Set(&tRangeStart2);
			tRangeEnd2.Add(&tRangeDiff);

			keys[i].Set(&tRangeStart2);
			if (i == 0) {
				printf("  Thread 00000: %064s ->", keys[i].GetBase16().c_str());
			}
			Int dobb;
			dobb.Set(&tRangeStart2);
			dobb.Add(&tRangeDiff);
			if (i == 0) {
				printf(" %064s \n", dobb.GetBase16().c_str());
			}
			if (i == 1) {
				printf("  Thread 00001: %064s -> %064s \n", tRangeStart2.GetBase16().c_str(), dobb.GetBase16().c_str());
			}
			if (i == 2) {
				printf("  Thread 00002: %064s -> %064s \n", tRangeStart2.GetBase16().c_str(), dobb.GetBase16().c_str());
			}
			if (i == 3) {
				printf("  Thread 00003: %064s -> %064s \n", tRangeStart2.GetBase16().c_str(), dobb.GetBase16().c_str());
				printf("          ... : \n");
			}
			if (i == nbThread - 2) {
				printf("  Thread %d: %064s -> %064s \n", i, tRangeStart2.GetBase16().c_str(), dobb.GetBase16().c_str());
			}
			if (i == nbThread - 1) {
				printf("  Thread %d: %064s -> %064s \n", i, tRangeStart2.GetBase16().c_str(), dobb.GetBase16().c_str());
			}
			if (i == nbThread) {
				printf("  Thread %d: %064s -> %064s \n\n", i, tRangeStart2.GetBase16().c_str(), dobb.GetBase16().c_str());
			}

			tRangeStart2.Add(&tRangeDiff);
			Int k(keys + i);
			k.Add((uint64_t)(groupSize / 2));	// Starting key is at the middle of the group
			p[i] = secp->ComputePublicKey(&k);
		}
	}
}

void Rotor::FindKeyGPU(TH_PARAM * ph)
{

	bool ok = true;

#ifdef WITHGPU

	// Global init
	int thId = ph->threadId;
	Int tRangeStart = ph->rangeStart;
	Int tRangeEnd = ph->rangeEnd;

	GPUEngine* g;
	switch (searchMode) {
	case (int)SEARCH_MODE_MA:
	case (int)SEARCH_MODE_MX:
		g = new GPUEngine(secp, ph->gridSizeX, ph->gridSizeY, ph->gpuId, maxFound, searchMode, compMode, coinType,
			BLOOM_N, bloom->get_bits(), bloom->get_hashes(), bloom->get_bf(), DATA, TOTAL_COUNT, (rKey != 0));
		break;
	case (int)SEARCH_MODE_SA:
		g = new GPUEngine(secp, ph->gridSizeX, ph->gridSizeY, ph->gpuId, maxFound, searchMode, compMode, coinType,
			hash160Keccak, (rKey != 0));
		break;
	case (int)SEARCH_MODE_SX:
		g = new GPUEngine(secp, ph->gridSizeX, ph->gridSizeY, ph->gpuId, maxFound, searchMode, compMode, coinType,
			xpoint, (rKey != 0));
		break;
	default:
		printf("  Invalid search mode format!");
		return;
		break;
	}


	int nbThread = g->GetNbThread();
	Point* p = new Point[nbThread];
	Int* keys = new Int[nbThread];
	std::vector<ITEM> found;

	printf("  GPU          : %s\n\n", g->deviceName.c_str());

	counters[thId] = 0;

	getGPUStartingKeys(tRangeStart, tRangeEnd, g->GetGroupSize(), nbThread, keys, p);
	ok = g->SetKeys(p);

	ph->hasStarted = true;
	ph->rKeyRequest = false;

	// GPU Thread
	while (ok && !endOfSearch) {

		if (ph->rKeyRequest) {
			getGPUStartingKeys(tRangeStart, tRangeEnd, g->GetGroupSize(), nbThread, keys, p);
			ok = g->SetKeys(p);
			ph->rKeyRequest = false;
		}

		// Call kernel
		switch (searchMode) {
		case (int)SEARCH_MODE_MA:
			ok = g->LaunchSEARCH_MODE_MA(found, false);
			for (int i = 0; i < (int)found.size() && !endOfSearch; i++) {
				ITEM it = found[i];
				if (coinType == COIN_BTC) {
					std::string addr = secp->GetAddress(it.mode, it.hash);
					if (checkPrivKey(addr, keys[it.thId], it.incr, it.mode)) {
						nbFoundKey++;
					}
				}
				else {
					std::string addr = secp->GetAddressETH(it.hash);
					if (checkPrivKeyETH(addr, keys[it.thId], it.incr)) {
						nbFoundKey++;
					}
				}
			}
			break;
		case (int)SEARCH_MODE_MX:
			ok = g->LaunchSEARCH_MODE_MX(found, false);
			for (int i = 0; i < (int)found.size() && !endOfSearch; i++) {
				ITEM it = found[i];
				//Point pk;
				//memcpy((uint32_t*)pk.x.bits, (uint32_t*)it.hash, 8);
				//string addr = secp->GetAddress(it.mode, pk);
				if (checkPrivKeyX(/*addr,*/ keys[it.thId], it.incr, it.mode)) {
					nbFoundKey++;
				}
			}
			break;
		case (int)SEARCH_MODE_SA:
			ok = g->LaunchSEARCH_MODE_SA(found, false);
			for (int i = 0; i < (int)found.size() && !endOfSearch; i++) {
				ITEM it = found[i];
				if (coinType == COIN_BTC) {
					std::string addr = secp->GetAddress(it.mode, it.hash);
					if (checkPrivKey(addr, keys[it.thId], it.incr, it.mode)) {
						nbFoundKey++;
					}
				}
				else {
					std::string addr = secp->GetAddressETH(it.hash);
					if (checkPrivKeyETH(addr, keys[it.thId], it.incr)) {
						nbFoundKey++;
					}
				}
			}
			break;
		case (int)SEARCH_MODE_SX:
			ok = g->LaunchSEARCH_MODE_SX(found, false);
			for (int i = 0; i < (int)found.size() && !endOfSearch; i++) {
				ITEM it = found[i];
				//Point pk;
				//memcpy((uint32_t*)pk.x.bits, (uint32_t*)it.hash, 8);
				//string addr = secp->GetAddress(it.mode, pk);
				if (checkPrivKeyX(/*addr,*/ keys[it.thId], it.incr, it.mode)) {
					nbFoundKey++;
				}
			}
			break;
		default:
			break;
		}

		if (ok) {
			for (int i = 0; i < nbThread; i++) {
				keys[i].Add((uint64_t)STEP_SIZE);
			}
			counters[thId] += (uint64_t)(STEP_SIZE)*nbThread; // Point
		}

	}

	delete[] keys;
	delete[] p;
	delete g;

#else
	ph->hasStarted = true;
	printf("  GPU code not compiled, use -DWITHGPU when compiling.\n");
#endif

	ph->isRunning = false;

}

// ----------------------------------------------------------------------------

bool Rotor::isAlive(TH_PARAM * p)
{

	bool isAlive = true;
	int total = nbCPUThread + nbGPUThread;
	for (int i = 0; i < total; i++)
		isAlive = isAlive && p[i].isRunning;

	return isAlive;

}

// ----------------------------------------------------------------------------

bool Rotor::hasStarted(TH_PARAM * p)
{

	bool hasStarted = true;
	int total = nbCPUThread + nbGPUThread;
	for (int i = 0; i < total; i++)
		hasStarted = hasStarted && p[i].hasStarted;

	return hasStarted;

}

// ----------------------------------------------------------------------------

uint64_t Rotor::getGPUCount()
{

	uint64_t count = 0;
	for (int i = 0; i < nbGPUThread; i++)
		count += counters[0x80L + i];
	return count;

}

// ----------------------------------------------------------------------------

uint64_t Rotor::getCPUCount()
{

	uint64_t count = 0;
	for (int i = 0; i < nbCPUThread; i++)
		count += counters[i];
	return count;

}

// ----------------------------------------------------------------------------

void Rotor::rKeyRequest(TH_PARAM * p) {

	int total = nbCPUThread + nbGPUThread;
	for (int i = 0; i < total; i++)
		p[i].rKeyRequest = true;

}
// ----------------------------------------------------------------------------

void Rotor::SetupRanges(uint32_t totalThreads)
{
	Int threads;
	threads.SetInt32(totalThreads);
	rangeDiff.Set(&rangeEnd);
	rangeDiff.Sub(&rangeStart);
	rangeDiff.Div(&threads);
}

// ----------------------------------------------------------------------------

void Rotor::Search(int nbThread, std::vector<int> gpuId, std::vector<int> gridSize, bool& should_exit)
{

	double t0;
	double t1;
	endOfSearch = false;
	nbCPUThread = nbThread;
	nbGPUThread = (useGpu ? (int)gpuId.size() : 0);
	nbFoundKey = 0;

	// setup ranges
	SetupRanges(nbCPUThread + nbGPUThread);

	memset(counters, 0, sizeof(counters));

	if (!useGpu)
		printf("\n");

	TH_PARAM* params = (TH_PARAM*)malloc((nbCPUThread + nbGPUThread) * sizeof(TH_PARAM));
	memset(params, 0, (nbCPUThread + nbGPUThread) * sizeof(TH_PARAM));

	// Launch CPU threads
	for (int i = 0; i < nbCPUThread; i++) {
		params[i].obj = this;
		params[i].threadId = i;
		params[i].isRunning = true;

		params[i].rangeStart.Set(&rangeStart);
		rangeStart.Add(&rangeDiff);
		params[i].rangeEnd.Set(&rangeStart);

#ifdef WIN64
		DWORD thread_id;
		CreateThread(NULL, 0, _FindKeyCPU, (void*)(params + i), 0, &thread_id);
		ghMutex = CreateMutex(NULL, FALSE, NULL);
#else
		pthread_t thread_id;
		pthread_create(&thread_id, NULL, &_FindKeyCPU, (void*)(params + i));
		ghMutex = PTHREAD_MUTEX_INITIALIZER;
#endif
	}

	// Launch GPU threads
	for (int i = 0; i < nbGPUThread; i++) {
		params[nbCPUThread + i].obj = this;
		params[nbCPUThread + i].threadId = 0x80L + i;
		params[nbCPUThread + i].isRunning = true;
		params[nbCPUThread + i].gpuId = gpuId[i];
		params[nbCPUThread + i].gridSizeX = gridSize[2 * i];
		params[nbCPUThread + i].gridSizeY = gridSize[2 * i + 1];

		params[nbCPUThread + i].rangeStart.Set(&rangeStart);
		rangeStart.Add(&rangeDiff);
		params[nbCPUThread + i].rangeEnd.Set(&rangeStart);


#ifdef WIN64
		DWORD thread_id;
		CreateThread(NULL, 0, _FindKeyGPU, (void*)(params + (nbCPUThread + i)), 0, &thread_id);
#else
		pthread_t thread_id;
		pthread_create(&thread_id, NULL, &_FindKeyGPU, (void*)(params + (nbCPUThread + i)));
#endif
	}

#ifndef WIN64
	setvbuf(stdout, NULL, _IONBF, 0);
#endif
	printf("\n");

	uint64_t lastCount = 0;
	uint64_t gpuCount = 0;
	uint64_t lastGPUCount = 0;

	// Key rate smoothing filter
#define FILTER_SIZE 8
	double lastkeyRate[FILTER_SIZE];
	double lastGpukeyRate[FILTER_SIZE];
	uint32_t filterPos = 0;

	double keyRate = 0.0;
	double gpuKeyRate = 0.0;
	char timeStr[256];

	memset(lastkeyRate, 0, sizeof(lastkeyRate));
	memset(lastGpukeyRate, 0, sizeof(lastkeyRate));

	// Wait that all threads have started
	while (!hasStarted(params)) {
		Timer::SleepMillis(500);
	}

	// Reset timer
	Timer::Init();
	t0 = Timer::get_tick();
	startTime = t0;
	Int p100;
	Int ICount;
	p100.SetInt32(100);
	double completedPerc = 0;
	uint64_t rKeyCount = 0;
	while (isAlive(params)) {

		int delay = 2000;
		while (isAlive(params) && delay > 0) {
			Timer::SleepMillis(500);
			delay -= 500;
		}

		gpuCount = getGPUCount();
		uint64_t count = getCPUCount() + gpuCount;
		ICount.SetInt64(count);
		int completedBits = ICount.GetBitLength();
		if (rKey <= 0) {
			completedPerc = CalcPercantage(ICount, rangeStart, rangeDiff2);
			//ICount.Mult(&p100);
			//ICount.Div(&this->rangeDiff2);
			//completedPerc = std::stoi(ICount.GetBase10());
		}

		t1 = Timer::get_tick();
		keyRate = (double)(count - lastCount) / (t1 - t0);
		gpuKeyRate = (double)(gpuCount - lastGPUCount) / (t1 - t0);
		lastkeyRate[filterPos % FILTER_SIZE] = keyRate;
		lastGpukeyRate[filterPos % FILTER_SIZE] = gpuKeyRate;
		filterPos++;

		// KeyRate smoothing
		double avgKeyRate = 0.0;
		double avgGpuKeyRate = 0.0;
		uint32_t nbSample;
		for (nbSample = 0; (nbSample < FILTER_SIZE) && (nbSample < filterPos); nbSample++) {
			avgKeyRate += lastkeyRate[nbSample];
			avgGpuKeyRate += lastGpukeyRate[nbSample];
		}
		avgKeyRate /= (double)(nbSample);
		avgGpuKeyRate /= (double)(nbSample);

		if (rKey > 0) {
			if (isAlive(params)) {
				rhex.Add(count);
				
				if (avgGpuKeyRate > 1000000000) {

					memset(timeStr, '\0', 256);
					printf("\r  [%s] [R: %llu] [%s] [F: %d] [CPU+GPU: %.2f Gk/s] [GPU: %.2f Gk/s] [T: %s]    ",
						toTimeStr(t1, timeStr),
						rKeyCount,
						rhex.GetBase16().c_str(),
						nbFoundKey,
						avgKeyRate / 1000000000.0,
						avgGpuKeyRate / 1000000000.0,
						formatThousands(count).c_str());
				}
				else {
					memset(timeStr, '\0', 256);
					printf("\r  [%s] [R: %llu] [%s] [F: %d] [CPU+GPU: %.2f Mk/s] [GPU: %.2f Mk/s] [T: %s]    ",
						toTimeStr(t1, timeStr),
						rKeyCount,
						rhex.GetBase16().c_str(),
						nbFoundKey,
						avgKeyRate / 1000000.0,
						avgGpuKeyRate / 1000000.0,
						formatThousands(count).c_str());
				}
			}
		}
		else {
			
			if (avgGpuKeyRate > 1000000000) {
				if (isAlive(params)) {
					memset(timeStr, '\0', 256);
					printf("\r  [%s] [CPU+GPU: %.2f Gk/s] [GPU: %.2f Gk/s] [C: %lf %%] [R: %llu] [T: %s (%d bit)] [F: %d]   ",
						toTimeStr(t1, timeStr),
						avgKeyRate / 1000000000.0,
						avgGpuKeyRate / 1000000000.0,
						completedPerc,
						rKeyCount,
						formatThousands(count).c_str(),
						completedBits,
						nbFoundKey);
				}
			}
			else {
				if (isAlive(params)) {
					memset(timeStr, '\0', 256);
					printf("\r  [%s] [CPU+GPU: %.2f Mk/s] [GPU: %.2f Mk/s] [C: %lf %%] [R: %llu] [T: %s (%d bit)] [F: %d]   ",
						toTimeStr(t1, timeStr),
						avgKeyRate / 1000000.0,
						avgGpuKeyRate / 1000000.0,
						completedPerc,
						rKeyCount,
						formatThousands(count).c_str(),
						completedBits,
						nbFoundKey);
				}
			}
		}
		
		
		if (rKey > 0) {
			if ((count - lastrKey) > (1000000000 * rKey)) {
				// rKey request
				rKeyRequest(params);
				lastrKey = count;
				rKeyCount++;
				rKeyCount2 += rKeyCount;
			}
		}

		lastCount = count;
		lastGPUCount = gpuCount;
		t0 = t1;
		if (should_exit || nbFoundKey >= targetCounter || completedPerc > 100.5)
			endOfSearch = true;
	}

	free(params);

	}

// ----------------------------------------------------------------------------

std::string Rotor::GetHex(std::vector<unsigned char> &buffer)
{
	std::string ret;

	char tmp[128];
	for (int i = 0; i < (int)buffer.size(); i++) {
		sprintf(tmp, "%02X", buffer[i]);
		ret.append(tmp);
	}
	return ret;
}

// ----------------------------------------------------------------------------
// ----------------------------------------------------------------------------
// ----------------------------------------------------------------------------

int Rotor::CheckBloomBinary(const uint8_t * _xx, uint32_t K_LENGTH)
{
	if (bloom->check(_xx, K_LENGTH) > 0) {
		uint8_t* temp_read;
		uint64_t half, min, max, current; //, current_offset
		int64_t rcmp;
		int32_t r = 0;
		min = 0;
		current = 0;
		max = TOTAL_COUNT;
		half = TOTAL_COUNT;
		while (!r && half >= 1) {
			half = (max - min) / 2;
			temp_read = DATA + ((current + half) * K_LENGTH);
			rcmp = memcmp(_xx, temp_read, K_LENGTH);
			if (rcmp == 0) {
				r = 1;  //Found!!
			}
			else {
				if (rcmp < 0) { //data < temp_read
					max = (max - half);
				}
				else { // data > temp_read
					min = (min + half);
				}
				current = min;
			}
		}
		return r;
	}
	return 0;
}

// ----------------------------------------------------------------------------

bool Rotor::MatchHash(uint32_t * _h)
{
	if (_h[0] == hash160Keccak[0] &&
		_h[1] == hash160Keccak[1] &&
		_h[2] == hash160Keccak[2] &&
		_h[3] == hash160Keccak[3] &&
		_h[4] == hash160Keccak[4]) {
		return true;
	}
	else {
		return false;
	}
}

// ----------------------------------------------------------------------------

bool Rotor::MatchXPoint(uint32_t * _h)
{
	if (_h[0] == xpoint[0] &&
		_h[1] == xpoint[1] &&
		_h[2] == xpoint[2] &&
		_h[3] == xpoint[3] &&
		_h[4] == xpoint[4] &&
		_h[5] == xpoint[5] &&
		_h[6] == xpoint[6] &&
		_h[7] == xpoint[7]) {
		return true;
	}
	else {
		return false;
	}
}

// ----------------------------------------------------------------------------

std::string Rotor::formatThousands(uint64_t x)
{
	char buf[32] = "";

	sprintf(buf, "%llu", x);

	std::string s(buf);

	int len = (int)s.length();

	int numCommas = (len - 1) / 3;

	if (numCommas == 0) {
		return s;
	}

	std::string result = "";

	int count = ((len % 3) == 0) ? 0 : (3 - (len % 3));

	for (int i = 0; i < len; i++) {
		result += s[i];

		if (count++ == 2 && i < len - 1) {
			result += ",";
			count = 0;
		}
	}
	return result;
}


// ----------------------------------------------------------------------------

char* Rotor::toTimeStr(int sec, char* timeStr)
{
	int h, m, s;
	h = (sec / 3600);
	m = (sec - (3600 * h)) / 60;
	s = (sec - (3600 * h) - (m * 60));
	sprintf(timeStr, "%0*d:%0*d:%0*d", 2, h, 2, m, 2, s);
	return (char*)timeStr;
}

// ----------------------------------------------------------------------------

//#include <gmp.h>
//#include <gmpxx.h>
// ((input - min) * 100) / (max - min)
//double Rotor::GetPercantage(uint64_t v)
//{
//	//Int val(v);
//	//mpz_class x(val.GetBase16().c_str(), 16);
//	//mpz_class r(rangeStart.GetBase16().c_str(), 16);
//	//x = x - mpz_class(rangeEnd.GetBase16().c_str(), 16);
//	//x = x * 100;
//	//mpf_class y(x);
//	//y = y / mpf_class(r);
//	return 0;// y.get_d();
//}




