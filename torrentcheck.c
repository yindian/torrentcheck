// torrentcheck.c
// Version 1.00 Mike Ingle <inglem@pobox.com> 2010-12-01
//
// To build:
// Linux/unix gcc
// You may have to experiment to get 64-bit fseek/ftell working.
// gcc -O torrentcheck.c sha1.c -o torrentcheck
// gcc -O -Dfopen=fopen64 -D_FILE_OFFSET_BITS=64 -DUSE_FTELLO torrentcheck.c sha1.c -o torrentcheck
//
// For win32 mingw "gcc version 4.5.0":
// gcc -O -D_FILE_OFFSET_BITS=64 -DUSE_FTELLO64 -DWIN32 torrentcheck.c sha1.c -o torrentcheck

#include <stdio.h>
#include <fcntl.h>
#include <stdlib.h>
#if 0
#include <malloc.h>
#endif
#include <string.h>

#ifndef UTF8MAC_DETECT
#define UTF8MAC_DETECT 1
#endif

#if UTF8MAC_DETECT
#include <strings.h>
#include <sys/param.h>

#ifndef __LITTLE_ENDIAN__
#if BYTE_ORDER == LITTLE_ENDIAN
#define __LITTLE_ENDIAN__ 1
#endif
#endif

typedef void *conv_t;

/* Our own notion of wide character, as UCS-4, according to ISO-10646-1. */
typedef unsigned int ucs4_t;

/* Return code if invalid input after a shift sequence of n bytes was read.
   (xxx_mbtowc) */
#define RET_SHIFT_ILSEQ(n)  (-1-2*(n))
/* Return code if invalid. (xxx_mbtowc) */
#define RET_ILSEQ           RET_SHIFT_ILSEQ(0)
/* Return code if only a shift sequence of n bytes was read. (xxx_mbtowc) */
#define RET_TOOFEW(n)       (-2-2*(n))
/* Retrieve the n from the encoded RET_... value. */
#define DECODE_SHIFT_ILSEQ(r)  ((unsigned int)(RET_SHIFT_ILSEQ(0) - (r)) / 2)
#define DECODE_TOOFEW(r)       ((unsigned int)(RET_TOOFEW(0) - (r)) / 2)
/* Maximum value of n that may be used as argument to RET_SHIFT_ILSEQ or RET_TOOFEW. */
#define RET_COUNT_MAX       ((INT_MAX / 2) - 1)

/* Return code if invalid. (xxx_wctomb) */
#define RET_ILUNI      -1
/* Return code if output buffer is too small. (xxx_wctomb, xxx_reset) */
#define RET_TOOSMALL   -2

#include "utf8mac.h"
#endif

// Begin required for SHA1
typedef unsigned char *POINTER;
typedef unsigned int UINT4;
typedef unsigned char BYTE;
typedef struct
{
	UINT4 digest[ 5 ];            /* Message digest */
	UINT4 countLo, countHi;       /* 64-bit bit count */
	UINT4 data[ 16 ];             /* SHS data buffer */
	int Endianness;
} SHA_CTX;

extern void SHAInit(SHA_CTX *);
extern void SHAUpdate(SHA_CTX *, BYTE *buffer, int count);
extern void SHAFinal(BYTE *output, SHA_CTX *);
#define SHA1_LEN 20
// End required for SHA1

// For filter mode only, torrent buffer length depends on the torrent
#define inputBufLen 262144

#ifdef NATIVE64BIT
typedef long int INT64;
#else
typedef long long INT64;
#endif

#ifdef USE_FTELLO64
#define tc_fseek fseeko64
#define tc_ftell ftello64
#else
#ifdef USE_FTELLO
#define tc_fseek fseeko
#define tc_ftell ftello
#else
#define tc_fseek fseek
#define tc_ftell ftell
#endif
#endif

#ifdef WIN32
#define DIR_SEPARATOR '\\'
#else
#define DIR_SEPARATOR '/'
#endif

typedef struct {
	char* filePath;
	INT64 numBytes;
	int errorsFound;
} fileRecord;

// Extracts the integer
// Returns the new offset into the input,
// or -1 if unable to parse the input
int beParseInteger(BYTE* benstr,int benstrLen,int benstrOffset,INT64* longOut) {
	INT64 foundValue = 0;
	INT64 negPos = 1;
	int i;
	BYTE b;
	for(i=benstrOffset;i<benstrLen;i++) {
		b = benstr[i];
		switch(b) {
			case 'i':
				if (i != benstrOffset) return (-1);
				break;
			case '0': case '1': case '2': case '3': case '4':
			case '5': case '6': case '7': case '8': case '9':
				foundValue = foundValue * 10 + (b - '0');
				break;
			case '-':
				negPos = -1;
				break;
			case 'e':
				*longOut = foundValue * negPos;
				return (i+1);
			default:
				return (-1);
		}
	}
	return (-1);
}


// Obtain the length of the string and a pointer to its beginning
// Returns the new offset into the input,
// or -1 if unable to parse the input
// Remember this is not a null terminated string. You need to memcpy and terminate it.
int beParseString(BYTE* benstr,int benstrLen,int benstrOffset,BYTE** stringBegin,int* stringLen) {
	int i;
	BYTE b;
	BYTE* foundString;
	int foundLen = 0;
	for(i=benstrOffset;i<benstrLen;i++) {
		b = benstr[i];
		switch(b) {
			case '0': case '1': case '2': case '3': case '4':
			case '5': case '6': case '7': case '8': case '9':
				foundLen = foundLen * 10 + (b - '0');
				break;
			case ':':
				if ((i + 1 + foundLen) > benstrLen) {
					return (-1);
				} else {
					*stringBegin = benstr + i + 1;
					*stringLen = foundLen;
					return (i + 1 + foundLen);
				}
			default:
				return (-1);
		}
	}
	return (-1);
}


//// Return offset of an element in a list, or -1 if not found
//int beFindInList(BYTE* benstr,int benstrLen,int benstrOffset,int listIndex) {
//	int i;
//	if ((benstrOffset < 0)||(benstrOffset >= benstrLen)) return (-1);
//	if (benstr[benstrOffset] != 'l') return (-1);
//	benstrOffset++;
//	if (benstr[benstrOffset] == 'e') return (-1);
//	for(i=0;i<listIndex;i++) {
//		benstrOffset = beStepOver(benstr,benstrLen,benstrOffset);
//		if ((benstrOffset < 0)||(benstrOffset >= benstrLen)) return (-1);
//	}
//	if (benstr[benstrOffset] == 'e') return (-1);
//	return (benstrOffset);
//}


int beStepOver(BYTE* benstr,int benstrLen,int benstrOffset);

// Return offset of an element in a dict, or -1 if not found
// dictKey is a null-terminated string
int beFindInDict(BYTE* benstr,int benstrLen,int benstrOffset,BYTE* dictKey) {
	BYTE* stringPtr;
	int stringLen;
	int dictKeyLen;

	if ((benstrOffset < 0)||(benstrOffset >= benstrLen)) return (-1);
	if (benstr[benstrOffset] != 'd') return (-1);
	dictKeyLen = strlen(dictKey);
	benstrOffset++;
	while ((benstrOffset >= 0)&&(benstrOffset < benstrLen)) {
		if (benstr[benstrOffset] == 'e') return (-1);
		benstrOffset = beParseString(benstr,benstrLen,benstrOffset,&stringPtr,&stringLen);
		if (benstrOffset < 0) return (-1);
		if ((stringLen == dictKeyLen) && (memcmp(stringPtr,dictKey,stringLen) == 0)) {
			return (benstrOffset);
		} else {
			benstrOffset = beStepOver(benstr,benstrLen,benstrOffset);
		}
	}
	return (-1);
}


// Step over an object (including all its embedded objects)
// Returns new offset, or -1 if unable to parse the input
int beStepOver(BYTE* benstr,int benstrLen,int benstrOffset) {
	BYTE* bp;
	int ip;
	if ((benstrOffset < 0)||(benstrOffset >= benstrLen)) return (-1);
	switch (benstr[benstrOffset]) {
		case 'i':
			benstrOffset ++;
			while(benstrOffset < benstrLen) {
				switch (benstr[benstrOffset]) {
					case '0': case '1': case '2': case '3': case '4':
					case '5': case '6': case '7': case '8': case '9':
					case '-':
						benstrOffset++;
						break;
					case 'e':
						benstrOffset++;
						if (benstrOffset < benstrLen) return benstrOffset;
						else return (-1);
					default:
						return (-1);
				}
			}
			return (-1);
		case '0': case '1': case '2': case '3': case '4':
		case '5': case '6': case '7': case '8': case '9':
			benstrOffset = beParseString(benstr,benstrLen,benstrOffset,&bp,&ip);
			if ((benstrOffset < 0) || (benstrOffset >= benstrLen)) return(-1);
			else return benstrOffset;
		case 'l':
		case 'd':
			benstrOffset++;
			while (benstrOffset < benstrLen) {
				if (benstr[benstrOffset] == 'e') {
					benstrOffset++;
					if (benstrOffset <= benstrLen) return benstrOffset;
					else return (-1);
				} else {
					benstrOffset = beStepOver(benstr,benstrLen,benstrOffset);
					if ((benstrOffset < 0) || (benstrOffset >= benstrLen)) return (-1);
				}
			}
			return (-1);
		default:
			return (-1);
	}
}


char* print64(INT64 val,char* buf,char useCommaDot) {
	INT64 divisor = 1000000000000000000l;
	char* bp = buf;
	int nonzero = 0;
	int digit;
	if (val < 0) {
		*bp = '-'; bp++;
		val = 0 - val;
	}
	while(divisor > 0) {
		digit = val / divisor;
		if (digit != 0) nonzero = 1;
		if (nonzero) {
			*bp = '0' + digit; bp++;
			if (useCommaDot&&((divisor == 1000l)||(divisor == 1000000l)||
                              (divisor == 1000000000l)||(divisor == 1000000000000l))) {
				*bp = useCommaDot; bp++;
			}
		}
		val -= digit * divisor;
		divisor /= 10;
	}
	if (nonzero == 0) {
		*bp = '0' + digit; bp++;
	}
	*bp = 0;
	return buf;
}


void backspaceProgressLine(int *showProgressChars) { // remove the progress line by backspacing
	if (*showProgressChars > 0) {
		fwrite("\
\010\010\010\010\010\010\010\010\010\010\010\010\010\010\010\010\
\010\010\010\010\010\010\010\010\010\010\010\010\010\010\010\010\
\010\010\010\010\010\010\010\010\010\010\010\010\010\010\010\010",*showProgressChars,1,stdout);
		*showProgressChars = 0;
	}
}


int sha1Filter(char* compareHash) {
	BYTE inputBuffer[inputBufLen];
	SHA_CTX sha1ctx;
	unsigned char sha1hash[SHA1_LEN];
	char hexHash[48];
	int bytesRead = 0;
#ifdef WIN32
	// Not portable, but then neither is Windows.
	_setmode(_fileno(stdin), _O_BINARY); // http://oldwiki.mingw.org/index.php/binary
#endif
	SHAInit(&sha1ctx);
	while(!feof(stdin)) {
		bytesRead = fread(inputBuffer,1,inputBufLen,stdin);
		SHAUpdate(&sha1ctx,inputBuffer,bytesRead);
	}
	SHAFinal(sha1hash,&sha1ctx);
	sprintf(hexHash,
        "%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x",
		(int)sha1hash[0], (int)sha1hash[1], (int)sha1hash[2], (int)sha1hash[3],
		(int)sha1hash[4], (int)sha1hash[5], (int)sha1hash[6], (int)sha1hash[7],
		(int)sha1hash[8], (int)sha1hash[9], (int)sha1hash[10], (int)sha1hash[11],
		(int)sha1hash[12], (int)sha1hash[13], (int)sha1hash[14], (int)sha1hash[15],
		(int)sha1hash[16], (int)sha1hash[17], (int)sha1hash[18], (int)sha1hash[19]);
	printf("%s\n",hexHash);
	if (compareHash == NULL) return 0;
	if (strcasecmp(hexHash,compareHash) == 0) return 0;
	return 1;
}


int main(int argc,char* argv[]) {
	char* torrentFile = NULL;
	char* contentPath = NULL;
	int contentPathLen = 0;
	FILE* fp;
	BYTE* torrent;
	int torrentLen;
	int i;
	INT64 bytesRead = -1;
	unsigned char sha1hash[SHA1_LEN];
	int torrentInfo = -1;
	int ofs = -1;
	int ofs2 = -1;
	int torrentPieceLen = -1;
	int lastFile = 0;
	int torrentFiles = -1;
	int thisFileOffset = -1;
	int multiFileTorrent = 0;
	int filterMode = 0;
	char* filterHash = NULL;
	int readLen;
	INT64 torrentPrivate = 0;
	INT64 fileBytesExpected = -1;
	INT64 fileBytesActual = -1;
	INT64 fileBytesRead = -1;
	BYTE* pieceList = NULL;
	int pieceListLen = -1;
	BYTE* pieceBuf = NULL;
	int bytesInBuf = 0;
	int bytesToRead = 0;
	BYTE* rootName = NULL;
	int rootNameLen = -1;
	BYTE* announce = NULL;
	int announceLen = -1;
	int numPieces = -1;
	int numFiles = 0;
	INT64 pieceLen = -1;
	BYTE* fileName = NULL;
	int fileNameLen = -1;
	char* filePath;
	char* filePath2;
	char* filePathActual;
	int filePathMax = 8192;
	int filePathOfs;
	INT64 totalBytes = 0;
	INT64 totalBytesDone = 0;
	int piecesDone = 0;
	int errorsFound = 0;
	int errorsFoundThisFile = 0;
	int piecesChecked = 0;
	int firstFileThisPiece = 0;
	int currentFile = 0;
	fileRecord* fileRecordList = NULL;
	int maxFileRecords = 16;
	int showProgressCount = 1;
	int showPieceDetail = 0;
	char useCommaDot = 0;
	int thisPieceBad = 0;
	int showProgressChars = 0;
	char progressBuf[48];
	char p64Buf1[32];
	char p64Buf2[32];
	SHA_CTX sha1ctx;

	// Check the build for a working hasher and correct word lengths
	SHAInit(&sha1ctx);
	SHAUpdate(&sha1ctx,"testing SHA1 hash",17);
	SHAFinal(sha1hash,&sha1ctx);
	if (memcmp(sha1hash,"\xac\x1f\xd0\xda\xea\x37\x65\x87\x9a\xde\xfa\x33\x38\x62\x71\xf3\x85\x08\xa8\xbd",SHA1_LEN) != 0) errorsFound++;
	SHAInit(&sha1ctx);
	SHAUpdate(&sha1ctx,"~}|{zyxwvutsrqponmlkjihgfedcba`_^]\\[ZYXWVUTSRQPONMLKJIHGFEDCBA@?>=<;:9876543210/.-,+*)('&%$#\"! !\"#$%&'()*+,-./0123456789:;<=>?@ABCDEFGHIJKLMNOPQRSTUVWXYZ[\\]^_`abcdefghijklmnopqrstuvwxyz{|}~",189);
	SHAFinal(sha1hash,&sha1ctx);
	if (memcmp(sha1hash,"\x11\xe5\x6b\x84\xd8\xda\xb8\x93\xcd\x8e\x2d\x85\xe4\x3c\xc0\x0d\x5a\xd1\xbb\x78",SHA1_LEN) != 0) errorsFound++;
	if (errorsFound > 0) {
		printf("SHA1 function test failed - this build is faulty!\n");
		return 3;
	}
	if ((sizeof(INT64) != 8) || (sizeof(UINT4) != 4)) {
		printf("Wrong word length UINT4=%lu INT64=%lu - this build is faulty!\n",sizeof(UINT4),sizeof(INT64));
		return 3;
	}

	for(i=1;i<argc;i++) {
		if ((strcmp(argv[i],"-t") == 0) && (i+1 < argc)) {
			i++; torrentFile = argv[i];
		} else if ((strcmp(argv[i],"-p") == 0) && (i+1 < argc)) {
			i++; contentPath = argv[i]; contentPathLen = strlen(contentPath);
		} else if (strcmp(argv[i],"-n") == 0) {
			showProgressCount = 0;
		} else if (strcmp(argv[i],"-h") == 0) {
			showProgressCount = 0;
			showPieceDetail = 1;
		} else if (strcmp(argv[i],"-c") == 0) {
			useCommaDot = ',';
		} else if (strcmp(argv[i],"-d") == 0) {
			useCommaDot = '.';
		} else if (strcmp(argv[i],"-sha1") == 0) {
			filterMode = 1;
			if (i+1 < argc) {
				i++;
				filterHash = argv[i];
			}
		} else {
			printf("Unrecognized option %s in command line at position %i\n",argv[i],i);
			torrentFile = NULL;
			break;
		}
	}

	// This just acts as a SHA1 filter, doing no torrent-related work
	if (filterMode) {
		return sha1Filter(filterHash);
	}

	if (torrentFile == NULL) {
		printf("torrentcheck - catalog a .torrent file and optionally verify content hashes\n");
		printf("Usage: torrentcheck -t torrent-file [-p content-path] [-n] [-h] [-c] [-d]\n");
		printf("Options: -n suppresses progress count, -h shows all hash values,\n");
		printf("         -c or -d uses comma or dot formatted byte counts.\n");
		printf("Returns 0 if successful, nonzero return code if errors found.\n");
		printf("\n");
		printf("Option: -sha1 [optional hash] acts as a simple SHA1 filter.\n");
		printf("If -sha1 is followed by a hex hash, the return code will be zero\n");
		printf("on match and nonzero otherwise.\n");
		printf("\n");
		printf("V1.00 2010-12-01 Mike Ingle <inglem@pobox.com>\n");
		return 3;
	}

	filePath = malloc(filePathMax);
	if (filePath == NULL) {
		printf("Unable to malloc %i bytes for file path\n",filePathMax);
		return 2;
	}

	fp = fopen(torrentFile,"rb");
	if (fp == NULL) {
		printf("Failed to open torrent metadata file %s\n",torrentFile);
		return 2;
	}

	tc_fseek(fp,0l,SEEK_END);
	torrentLen = (int) tc_ftell(fp);
	rewind(fp);
	if (torrentLen > 16777216) {
		printf("Torrent metadata file %s is %i bytes long.\n",torrentFile,torrentLen);
		printf("That is unusually large for a torrent file. You may have specified an\n");
		printf("incorrect file. The metadata must be loaded into memory, so this may\n");
		printf("take some time or fail due to lack of memory.\n");
		printf("\n");
	}

	torrent = malloc(torrentLen);
	if (torrent == NULL) {
		printf("Unable to malloc %i bytes for torrent metadata\n",torrentLen);
		return 2;
	}
	bytesRead = fread(torrent,1,torrentLen,fp);
	if (fp != NULL) fclose(fp); fp = NULL;
	i = beStepOver(torrent,torrentLen,0);
	if (i == -1) {
		printf("Unable to parse torrent metadata file %s\n",torrentFile);
		return 2;
	}
	if (i != torrentLen) {
	  printf("Torrent metadata file %s has some inconsistencies with its size\n",torrentFile);
	}

	torrentInfo = beFindInDict(torrent,torrentLen,0,"info");
	if (torrentInfo < 0) {
		printf("Unable to read \"info\" from torrent\n");
		return 2;
	}

	ofs = beFindInDict(torrent,torrentLen,torrentInfo,"name");
	if (ofs >= 0) {
		ofs = beParseString(torrent,torrentLen,ofs,&rootName,&rootNameLen);
	}
	if (ofs < 0) {
		printf("Unable to read \"name\" from torrent\n");
		return 2;
	}

	ofs = beFindInDict(torrent,torrentLen,torrentInfo,"private");
	if (ofs >= 0) {
 		ofs = beParseInteger(torrent,torrentLen,ofs,&torrentPrivate);
	}

	ofs = beFindInDict(torrent,torrentLen,torrentInfo,"length");
	if (ofs >= 0) { // single file
 		ofs = beParseInteger(torrent,torrentLen,ofs,&fileBytesExpected);
		totalBytes = fileBytesExpected;
	} else { // multi file
		multiFileTorrent = 1;
		torrentFiles = beFindInDict(torrent,torrentLen,torrentInfo,"files");

		// Count files
		thisFileOffset = torrentFiles;
		if ((thisFileOffset >= 0)&&(thisFileOffset < torrentLen-1) &&
            (torrent[thisFileOffset] == 'l')) thisFileOffset++;
		while ((thisFileOffset >= 0)&&(thisFileOffset < torrentLen)) {
			if (torrent[thisFileOffset] != 'd') break;
			thisFileOffset = beStepOver(torrent,torrentLen,thisFileOffset);
			numFiles ++;
		}

		fileRecordList = malloc(numFiles * sizeof(fileRecord));
		if (torrent == NULL) {
			printf("Unable to malloc %lu bytes for file record list\n",numFiles * sizeof(fileRecord));
			return 2;
		}

		// Catalog individual files and sum length
		thisFileOffset = torrentFiles;
		currentFile = 0;
		if ((thisFileOffset >= 0)&&(thisFileOffset < torrentLen-1) &&
            (torrent[thisFileOffset] == 'l')) thisFileOffset++;
		while ((thisFileOffset >= 0)&&(thisFileOffset < torrentLen)) {
			if (torrent[thisFileOffset] != 'd') break;
			ofs = beFindInDict(torrent,torrentLen,thisFileOffset,"length");
			if (ofs >= 0) {
 				ofs = beParseInteger(torrent,torrentLen,ofs,&fileBytesExpected);
			}
			if (ofs < 0) {
				printf("Unable to read \"length\" from torrent\n");
				return 2;
			}
			ofs = beFindInDict(torrent,torrentLen,thisFileOffset,"path.utf-8");
			if (ofs < 0) {
				ofs = beFindInDict(torrent,torrentLen,thisFileOffset,"path");
			}
			if (ofs < 0) {
				printf("Unable to read \"path\" from torrent\n");
				return 2;
			}

			filePathOfs = 0;
			if (torrent[ofs] == 'l') ofs++;
			while((ofs>=0) && (ofs<torrentLen) && (torrent[ofs] != 'e')) {
				ofs = beParseString(torrent,torrentLen,ofs,&fileName,&fileNameLen);
				if (ofs < 0) {
					printf("Unable to read \"path\" from torrent\n");
					return 2;
				}
				if (filePathOfs != 0) {
					filePath[filePathOfs] = DIR_SEPARATOR;
					filePathOfs++;
				}
				while (fileNameLen + filePathOfs + contentPathLen + rootNameLen + 16 >= filePathMax) {
					filePathMax *= 2;
					filePath = realloc(filePath,filePathMax);
					if (filePath == NULL) {
						printf("Unable to realloc %i bytes for file path\n",filePathMax);
						return 2;
					}
				}
				memcpy(filePath+filePathOfs,fileName,fileNameLen);
				filePathOfs += fileNameLen;
				filePath[filePathOfs] = 0;
			}

			fileRecordList[currentFile].filePath = malloc(strlen(filePath)+1);
			if (fileRecordList[currentFile].filePath == NULL) {
				printf("Unable to malloc %lu bytes for file path\n",strlen(filePath)+1);
				return 2;
			}
			strcpy(fileRecordList[currentFile].filePath,filePath);
			fileRecordList[currentFile].numBytes = fileBytesExpected;
			fileRecordList[currentFile].errorsFound = 0;

			thisFileOffset = beStepOver(torrent,torrentLen,thisFileOffset);
			totalBytes += fileBytesExpected;
			currentFile ++;
		}
	}

	filePath2 = malloc(filePathMax);
	if (filePath2 == NULL) {
		printf("Unable to malloc %i bytes for file path (2)\n",filePathMax);
		return 2;
	}

	if ((ofs < 0)&&(torrentFiles < 0)) {
		printf("Unable to read \"info->length\" or \"info->files\" from torrent\n");
		return 2;
	}

	ofs = beFindInDict(torrent,torrentLen,torrentInfo,"pieces");
	if (ofs >= 0) {
		ofs = beParseString(torrent,torrentLen,ofs,&pieceList,&pieceListLen);
	}
	if (ofs < 0) {
		printf("Unable to read \"pieces\" from torrent\n");
		return 2;
	}

	numPieces = pieceListLen / SHA1_LEN;
	if (numPieces * SHA1_LEN != pieceListLen) {
		printf("Pieces list length is not a multiple of 20\n");
		return 2;
	}

	ofs = beFindInDict(torrent,torrentLen,torrentInfo,"piece length");
	if (ofs >= 0) {
 		ofs = beParseInteger(torrent,torrentLen,ofs,&pieceLen);
	}
	if (ofs < 0) {
		printf("Unable to read \"piece length\" from torrent\n");
		return 2;
	}

	printf("Torrent file  : %s\n",torrentFile);
	printf("Metadata info : %i bytes, %i piece%s, %s bytes per piece%s\n",torrentLen,numPieces,((numPieces==1)?"":"s"),print64(pieceLen,p64Buf1,useCommaDot),((torrentPrivate==1)?", private":""));
	printf("Torrent name  : ");
	fwrite(rootName,rootNameLen,1,stdout);
	printf("\n");

	if (multiFileTorrent) {
		printf("Content info  : %i file%s, %s bytes\n",numFiles,((numFiles==1)?"":"s"),print64(totalBytes,p64Buf1,useCommaDot));
	} else {
		printf("Content info  : single file, %s bytes\n",print64(totalBytes,p64Buf1,useCommaDot));
	}

	ofs = beFindInDict(torrent,torrentLen,0,"announce");
	if (ofs >= 0) {
		ofs = beParseString(torrent,torrentLen,ofs,&announce,&announceLen);
	}
	if (ofs >= 0) {
		printf("Announce URL  : ");
		fwrite(announce,announceLen,1,stdout);
		printf("\n");
	}

	if ((multiFileTorrent)&&(contentPath == NULL)) {
		printf("\n");
		if (useCommaDot) {
			printf("F#  Bytes         File name\n");
			printf("--- ------------- -------------------------------------------------------------\n");
		} else {
			printf("F#  Bytes       File name\n");
			printf("--- ----------- ---------------------------------------------------------------\n");
		}
		for(i=0;i<numFiles;i++) {
			printf(useCommaDot?"%3i %13s %s\n":"%3i %11s %s\n",i+1,print64(fileRecordList[i].numBytes,p64Buf1,useCommaDot),fileRecordList[i].filePath);
		}
		printf("\n");
	}

	if (contentPath != NULL) {
		pieceBuf = malloc(pieceLen);
		if (pieceBuf == NULL) {
			printf("Unable to malloc %lli bytes for piece buffer\n",pieceLen);
			return 2;
		}
	}

    /////////////////////////////////////////////////////////////////////////////////////
	if ((multiFileTorrent)&&(contentPath != NULL)) { // multi-file torrent check
		printf("\n");
		if (useCommaDot) {
			printf("F#  Ok? Bytes         File name\n");
			printf("--- --- ------------- ---------------------------------------------------------\n");
		} else {
			printf("F#  Ok? Bytes       File name\n");
			printf("--- --- ----------- -----------------------------------------------------------\n");
		}

		thisFileOffset = torrentFiles;
		bytesRead = 0;
		currentFile = 0;
		while (currentFile < numFiles) {

			memcpy(filePath,contentPath,contentPathLen);
			filePathOfs = contentPathLen;
			if ((filePathOfs > 0)&&(filePath[filePathOfs-1] != DIR_SEPARATOR)) {
				filePath[filePathOfs] = DIR_SEPARATOR;
				filePathOfs++;
			}
			memcpy(filePath + filePathOfs,rootName,rootNameLen);
			filePathOfs += rootNameLen;
			if ((filePathOfs > 0)&&(filePath[filePathOfs-1] != DIR_SEPARATOR)) {
				filePath[filePathOfs] = DIR_SEPARATOR;
				filePathOfs++;
			}
			strcpy(filePath+filePathOfs,fileRecordList[currentFile].filePath);

			fileBytesExpected = fileRecordList[currentFile].numBytes;

			filePathActual = filePath;
			fp = fopen(filePath,"rb");
			if (fp == NULL) { // Try without root path
				memcpy(filePath2,contentPath,contentPathLen);
				filePathOfs = contentPathLen;
				if ((filePathOfs > 0)&&(filePath2[filePathOfs-1] != DIR_SEPARATOR)) {
					filePath2[filePathOfs] = DIR_SEPARATOR;
					filePathOfs++;
				}
				strcpy(filePath2+filePathOfs,fileRecordList[currentFile].filePath);
				filePathActual = filePath2;
				fp = fopen(filePath2,"rb");
				if (fp == NULL) { // Generate padding files on-the-fly
					if (!strncmp(fileRecordList[currentFile].filePath, "_____padding_file_", 18)) {
						fp = tmpfile();
						if (fp) {
							INT64 n = fileBytesExpected;
							while (n > 0) {
								fputc(0, fp);
								--n;
							}
							rewind(fp);
						}
					}
				}
#if UTF8MAC_DETECT
				if (fp == NULL) {
					const char *s = fileRecordList[currentFile].filePath;
					char *p = filePath2+filePathOfs;
					size_t len = strlen(s);
					size_t n = filePathMax - filePathOfs - 1;
					ucs4_t wc;
					int ret;
#define CONV_S_TO_P_IN_UTF8MAC() \
					while (len) { \
						ret = utf8mac_mbtowc(NULL, &wc, s, len); \
						if (ret <= 0 || len < (size_t) ret) \
							break; \
						s += ret; \
						len -= ret; \
						ret = utf8mac_wctomb(NULL, p, wc, n); \
						if (ret <= 0 || n < (size_t) ret) \
							break; \
						p += ret; \
						n -= ret; \
					}
					CONV_S_TO_P_IN_UTF8MAC();
					if (len == 0) {
						*p = 0;
						fp = fopen(filePath2, "rb");
						if (fp == NULL) {
							s = contentPath;
							p = filePath2;
							len = contentPathLen;
							n = filePathMax - 1;
							CONV_S_TO_P_IN_UTF8MAC();
							if (len == 0) {
								if (p > filePath2 && p[-1] != DIR_SEPARATOR) {
									*p++ = DIR_SEPARATOR;
									--n;
								}
								s = fileRecordList[currentFile].filePath;
								len = strlen(s);
								CONV_S_TO_P_IN_UTF8MAC();
								if (len == 0) {
									*p = 0;
									fp = fopen(filePath2, "rb");
								}
							}
						}
					}
#undef CONV_S_TO_P_IN_UTF8MAC
					if (fp == NULL) {
						memcpy(filePath2,contentPath,contentPathLen);
						filePathOfs = contentPathLen;
						if ((filePathOfs > 0)&&(filePath2[filePathOfs-1] != DIR_SEPARATOR)) {
							filePath2[filePathOfs] = DIR_SEPARATOR;
							filePathOfs++;
						}
						strcpy(filePath2+filePathOfs,fileRecordList[currentFile].filePath);
					}
				}
#endif
			}

			if (fp == NULL) {
				backspaceProgressLine(&showProgressChars);
				printf("Unable to open file %s or %s\n",filePath,filePath2);
				errorsFound++;
				errorsFoundThisFile++;
			} else {
				tc_fseek(fp,0l,SEEK_END);
				fileBytesActual = tc_ftell(fp);
				rewind(fp);
				if (fileBytesActual != fileBytesExpected) {
					backspaceProgressLine(&showProgressChars);
					printf("File %s length mismatch, expected %s bytes, found %s bytes\n",filePathActual,print64(fileBytesExpected,p64Buf1,useCommaDot),print64(fileBytesActual,p64Buf2,useCommaDot));
					errorsFound++;
					errorsFoundThisFile++;
				}
			}
			fileBytesRead = 0;
			while(fileBytesRead < fileBytesExpected) {
				if (fileBytesExpected - fileBytesRead < pieceLen - bytesRead) {
					bytesToRead = fileBytesExpected - fileBytesRead;
				} else {
					bytesToRead = pieceLen - bytesRead;
				}
				if (fp != NULL) readLen = fread(pieceBuf+bytesRead,1,bytesToRead,fp);
				else readLen = 0;
				bytesRead += bytesToRead; fileBytesRead += bytesToRead;
				if ((fp != NULL)&&(readLen != bytesToRead)) {
					backspaceProgressLine(&showProgressChars);
					printf("Short read, got %i bytes, expected %i bytes at offset %s of %s\n",readLen,bytesToRead,print64(fileBytesRead,p64Buf1,useCommaDot),filePathActual);
					errorsFound ++;
					errorsFoundThisFile ++;
				}
				if (currentFile + 1 >= numFiles) lastFile = 1;
				if ((bytesRead == pieceLen)||((lastFile==1)&&(fileBytesRead == fileBytesExpected))) {

				    if ((fp != NULL)&&(readLen == bytesToRead)) {
						SHAInit(&sha1ctx);
						SHAUpdate(&sha1ctx,pieceBuf,bytesRead);
						SHAFinal(sha1hash,&sha1ctx);
					}
					totalBytesDone += bytesRead;
					i = piecesDone * SHA1_LEN;

					if ((fp == NULL)||(readLen != bytesToRead)) {
						errorsFound ++;
						errorsFoundThisFile ++;
						thisPieceBad = 2;
						for(i=firstFileThisPiece;i<=currentFile;i++) {
							fileRecordList[i].errorsFound = errorsFoundThisFile;
						}
					} else if (memcmp(pieceList+i,sha1hash,SHA1_LEN) != 0) {
						errorsFound ++;
						errorsFoundThisFile ++;
						thisPieceBad = 1;
						for(i=firstFileThisPiece;i<=currentFile;i++) {
							fileRecordList[i].errorsFound = errorsFoundThisFile;
						}
					}

					if (thisPieceBad || showPieceDetail) {
						backspaceProgressLine(&showProgressChars);
						if ((showPieceDetail == 1)||(thisPieceBad == 1)) {
							printf("piece %i computed SHA1 %02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x\n",piecesDone,
								(int)sha1hash[0], (int)sha1hash[1], (int)sha1hash[2], (int)sha1hash[3],
								(int)sha1hash[4], (int)sha1hash[5], (int)sha1hash[6], (int)sha1hash[7],
								(int)sha1hash[8], (int)sha1hash[9], (int)sha1hash[10], (int)sha1hash[11],
								(int)sha1hash[12], (int)sha1hash[13], (int)sha1hash[14], (int)sha1hash[15],
								(int)sha1hash[16], (int)sha1hash[17], (int)sha1hash[18], (int)sha1hash[19]);

							printf("piece %i expected SHA1 %02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x\n",piecesDone,
								(int)pieceList[i+0], (int)pieceList[i+1], (int)pieceList[i+2], (int)pieceList[i+3],
								(int)pieceList[i+4], (int)pieceList[i+5], (int)pieceList[i+6], (int)pieceList[i+7],
								(int)pieceList[i+8], (int)pieceList[i+9], (int)pieceList[i+10], (int)pieceList[i+11],
								(int)pieceList[i+12], (int)pieceList[i+13], (int)pieceList[i+14], (int)pieceList[i+15],
								(int)pieceList[i+16], (int)pieceList[i+17], (int)pieceList[i+18], (int)pieceList[i+19]);
						}
						printf("piece %i is files %i-%i, %s bytes, %s total bytes, %i error%s\n",piecesDone,firstFileThisPiece+1,currentFile+1,print64(bytesRead,p64Buf1,useCommaDot),print64(totalBytesDone,p64Buf2,useCommaDot),errorsFound,((errorsFound == 1)?"":"s"));
						thisPieceBad = 0;
					}

					for(i=firstFileThisPiece;i<currentFile;i++) {
						backspaceProgressLine(&showProgressChars);
						printf(useCommaDot?"%3i %-3s %13s %s\n":"%3i %-3s %11s %s\n",i+1,((fileRecordList[i].errorsFound == 0)?"ok":"BAD"),
                                                    print64(fileRecordList[i].numBytes,p64Buf1,useCommaDot),fileRecordList[i].filePath);
					}

					piecesDone ++;
					firstFileThisPiece = currentFile;
					bytesRead = 0;
					if (showProgressCount) {
						backspaceProgressLine(&showProgressChars);
						sprintf(progressBuf,"%3i %s %i/%i (%i%%) ",currentFile+1,((errorsFound == 0)?"ok":"BAD"),piecesDone,numPieces,100*piecesDone/numPieces);
						showProgressChars = strlen(progressBuf);
						fwrite(progressBuf,showProgressChars,1,stdout);
						fflush(stdout);
					}
				}
			}
			if (fp != NULL) fclose(fp); fp = NULL;
			errorsFoundThisFile = 0;
			currentFile ++;
		}

		for(i=firstFileThisPiece;i<currentFile;i++) {
			backspaceProgressLine(&showProgressChars);
			printf(useCommaDot?"%3i %-3s %13s %s\n":"%3i %-3s %11s %s\n",i+1,((fileRecordList[i].errorsFound == 0)?"ok":"BAD"),
                                        print64(fileRecordList[i].numBytes,p64Buf1,useCommaDot),fileRecordList[i].filePath);
		}

		if (fp != NULL) fclose(fp); fp = NULL;
		backspaceProgressLine(&showProgressChars);
		printf("Total files %i, total bytes %s, total errors %i, %s\n",currentFile,print64(totalBytesDone,p64Buf1,useCommaDot),errorsFound, (errorsFound == 0)?"torrent is good":"torrent has errors");
		if (errorsFound > 0) {
			return 1;
		} else {
			return 0;
		}

    /////////////////////////////////////////////////////////////////////////////////////
	} else if (contentPath != NULL) { // single file torrent
		strcpy(filePath,contentPath);
		filePathOfs = strlen(filePath);
		if ((filePathOfs > 0)&&(filePath[filePathOfs-1] != DIR_SEPARATOR)) {
			filePath[filePathOfs] = DIR_SEPARATOR;
			filePathOfs++;
		}
		memcpy(filePath + filePathOfs,rootName,rootNameLen);
		filePath[filePathOfs+rootNameLen] = 0;

		fp = fopen(filePath,"rb");
		if (fp == NULL ) {
			fp = fopen(contentPath,"rb");
			if (fp != NULL) strcpy(filePath,contentPath);
		}
		if (fp == NULL) {
			printf("Unable to open file %s or %s\n",contentPath,filePath);
			return 2;
		}

		tc_fseek(fp,0l,SEEK_END);
		fileBytesActual = tc_ftell(fp);
		rewind(fp);
		if (fileBytesActual != fileBytesExpected) {
			backspaceProgressLine(&showProgressChars);
			printf("File length mismatch, expected %s bytes, found %s bytes\n",print64(fileBytesExpected,p64Buf1,useCommaDot),print64(fileBytesActual,p64Buf2,useCommaDot));
			errorsFound++;
		}

		for(piecesDone = 0;piecesDone<numPieces;piecesDone++) {

			bytesRead = fread(pieceBuf,1,pieceLen,fp);
			SHAInit(&sha1ctx);
			SHAUpdate(&sha1ctx,pieceBuf,bytesRead);
			SHAFinal(sha1hash,&sha1ctx);
			totalBytesDone += bytesRead;
			i = piecesDone * SHA1_LEN;

			if (memcmp(pieceList+i,sha1hash,SHA1_LEN) != 0) {
				errorsFound ++;
				thisPieceBad = 1;
			}

			if (thisPieceBad || showPieceDetail) {
				backspaceProgressLine(&showProgressChars);
				printf("piece %i computed SHA1 %02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x\n",piecesDone,
					(int)sha1hash[0], (int)sha1hash[1], (int)sha1hash[2], (int)sha1hash[3],
					(int)sha1hash[4], (int)sha1hash[5], (int)sha1hash[6], (int)sha1hash[7],
					(int)sha1hash[8], (int)sha1hash[9], (int)sha1hash[10], (int)sha1hash[11],
					(int)sha1hash[12], (int)sha1hash[13], (int)sha1hash[14], (int)sha1hash[15],
					(int)sha1hash[16], (int)sha1hash[17], (int)sha1hash[18], (int)sha1hash[19]);

				printf("piece %i expected SHA1 %02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x\n",piecesDone,
					(int)pieceList[i+0], (int)pieceList[i+1], (int)pieceList[i+2], (int)pieceList[i+3],
					(int)pieceList[i+4], (int)pieceList[i+5], (int)pieceList[i+6], (int)pieceList[i+7],
					(int)pieceList[i+8], (int)pieceList[i+9], (int)pieceList[i+10], (int)pieceList[i+11],
					(int)pieceList[i+12], (int)pieceList[i+13], (int)pieceList[i+14], (int)pieceList[i+15],
					(int)pieceList[i+16], (int)pieceList[i+17], (int)pieceList[i+18], (int)pieceList[i+19]);
				printf("piece %i %s bytes, %s total bytes, %i error%s\n",piecesDone,print64(bytesRead,p64Buf1,useCommaDot),print64(totalBytesDone,p64Buf2,useCommaDot),errorsFound,((errorsFound == 1)?"":"s"));
				thisPieceBad = 0;
			}

			if (showProgressCount) {
				backspaceProgressLine(&showProgressChars);
				sprintf(progressBuf,"%s %i/%i (%i%%) ",((errorsFound == 0)?"ok":"BAD"),piecesDone,numPieces,100*piecesDone/numPieces);
				showProgressChars = strlen(progressBuf);
				fwrite(progressBuf,showProgressChars,1,stdout);
				fflush(stdout);
			}

		}
		if (fp != NULL) fclose(fp); fp = NULL;

		backspaceProgressLine(&showProgressChars);
		printf("Total bytes %s, total errors %i, %s\n",print64(totalBytesDone,p64Buf1,useCommaDot),errorsFound, (errorsFound == 0)?"torrent is good":"torrent has errors");

		if (errorsFound > 0) {
			return 1;
		} else {
			return 0;
		}
	}


	return 2; // How did we get here?
}


// EOF
