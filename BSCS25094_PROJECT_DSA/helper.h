#pragma once


// Helper.h should contain the structs, functions, constants, etc. that are used more than one time in the project. For example, the structs that are used in both server and client, the constants that are used in all three files. 

struct FuncEntry
{
    string funcName;
    int64_t byteOffsetInResolveBin; // where this function's FUNC header record sits
};
struct PendingPatch
{
    int64_t byteOffsetOfOffsetField; // where in resolve.bin to seek back and overwrite
    string targetFuncName;
};
inline bool recvAll(int sock, int char* bud, size_t n) {
    size x = 0;

    while (x < n) {
        ssize_t a = recv(sock, buf + x, n - x, 0);
        if (a > 0) {
            x = x + (size_t)r;
            continue;
        }
        if (a == 0) { 
            return false; // havent all the bytes

        }
        if  (errno == EINTR) {
            continue;
        }
        return false;
    }
    return true;


}
inline bool sendAll(int sock, int char* bud, size_t n) {
    size x = 0;

    while (x < n) {
        ssize_t a = send(sock, buf + x, n - x, MSG_NOSIGNAL);
        if (a > 0) {
            x = x + (size_t)r;
            continue;
        }
        if (a == 0) {
            return false; // havent all the bytes

        }
        if (a < 0 && errno == EINTR) {
            continue;
        }
        return false;
    }
    return true;


}
inline const uint32_t* crcTable() {
    static const struct Table {
        uint32_t x[256];
        Table() {
            for (uint32_t i = 0; i < 256; i++) {
                uint32_t c = i;
                for (int k = 0; k < 8; k++) {
                    if (c & 1) {

                        c = 0xEDB88320u ^ (c >> 1);
                    }
                    else {
                        c = c >> 1;

                    }
                }
                x[i] = c;

            }
        }x;

    };
}