// ======================= TIME-TRAVEL DEBUGGER - CLIENT TEMPLATE =======================

// The client never talks to the server directly. It only talks to the load balancer.
// The load balancer wakes the server up if it is down, so the client does not care.
// The first "start" may just take a bit longer.
//
// The network is used ONLY by "start". After the .tdbg file is downloaded,
// next / prv / jmp read that file on the local disk. They never contact the load balancer.
//
// How the pieces fit together:
//   main loop -> readCommand -> runCommand
//   start -> cmdStart()  -> requestDebugFile() (Part A) -> openTdbg() (Part B) -> shows step 1
//   next, prv, jmp -> moveBy() (Part D) -> readSnapshotAt() (Part B) -> printSnapshot() (Part E)

#include <iostream>
#include <string>
#include <cstdint>
#include <sstream>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unistd.h>
#include <signal.h>
#include <vector>
#include <sys/socket.h>
#include <cstdint>
#include <cerrno>
#include <sys/time.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include "helper.h"
using namespace std;

// ---- Constants ----
const char* PROGRAM_NAME = "ttdb";
const char* PROMPT = "ttdb> ";
/*const int IO_BUFFER_SIZE = 64 * 1024; */    // fixed buffer for streaming files to and from the socket
const char* TDBG_FILE = "ttdb_last.tdbg"; // where the downloaded .tdbg is saved
const int MAX_JUMP = 1000000000;          // biggest n accepted by jmp
const int32_t TTDB_VERSION = 1;
string LB_HOST = DEFAULT_HOST;
int LB_PORT = 5000;
const char* TDBG_TMP_FILE = "ttdb_last.tdbg.tmp";  
const int32_t MAX_STRING_LENGTH = 1000000;

// These two must be the same as in Phase 1

//const int MAX_VARS_PER_FRAME = 16;
//const int MAX_STACK_DEPTH = 64;


// ---- Structs (same as Phase 1, so the file can be read back exactly as it was written) ----
//struct Variable
//{
//    string name;
//    int32_t value;
//};
//struct Frame
//{
//    string func_name;
//    int32_t argc;
//    Variable argv[MAX_VARS_PER_FRAME];
//    int32_t returnLine;
//    Variable locals[MAX_VARS_PER_FRAME];
//    int32_t localCount;
//};
//struct Snapshot
//{
//    Frame callStack[MAX_STACK_DEPTH];
//    int32_t stackDepth;
//};
//struct TTDBHeader
//{
//    char magic[4]; // "TTDB"
//    int32_t version;
//    int32_t stepCount;
//    int64_t indexOffset;
//};

// ---- Structs only the client needs ----
struct TdbgReader
{
    FILE* file;
    TTDBHeader header;
    vector<int64_t> index;
};
struct DebugSession
{
    int active_flag;     // false until a "start" has worked
    TdbgReader reader;   // the .tdbg file stays open for the whole session
    int64_t currentStep;
};

// =============================================================================================
// PART A: TALKING TO THE LOAD BALANCER
// =============================================================================================

//bool recvAll(int sock, char* buf, size_t n)
//{
//    // recv() can return fewer bytes than asked for, so keep calling it until exactly n bytes are read.
//    // Return false if the connection closes or fails before n bytes arrive.
//}
//bool sendAll(int sock, const char* buf, size_t n)
//{
//    // send() can send fewer bytes than asked for, so keep calling it until all n bytes are sent.
//    // Use the flag MSG_NOSIGNAL. Return false if the connection fails.
//}

string trimEnd(string t){
    while (!t.empty() && (t.back() == '\n' || t.back() == '\r' || t.back() == ' ')){

    
        t.pop_back();
    }


    return t;


}
int connectToHost(const char* host, int port)
{

    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0){
    
        return -1;
    }
    sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons((uint16_t)port);

    if (inet_pton(AF_INET, host, &addr.sin_addr) != 1){

        close(sock);

        return -1;
    }
    if (connect(sock, ( sockaddr*)&addr, sizeof(addr)) <  0){

        close(sock);
        return -1;
    }

    return sock;
    // Create a TCP socket and connect it to host:port.
    // Return the socket, or -1 if the connection fails.
}
bool uploadSource(int sock, const char* sourcePath)
{
    bool smt = sendFramedFile(sock, STATUS_OK, sourcePath);

    shutdown(sock, SHUT_WR);
    return smt;
    // have to do checksum and then send
    // When the file is finished, call shutdown(sock, SHUT_WR). This is how the server knows the string is complete.
    // Return false if the file cannot be opened or any send fails.
}

bool receiveResponse(int socket, const char* outputFilePath, string& errorMessage) {

    TrailerChecker crcChecker;
    vector<char> receivedBuffer(IO_BUFFER_SIZE);

    string receivedData;
    string serverError;
    FILE* outputFile = nullptr;
    bool statusReceived = false;
    uint8_t responseStatus = STATUS_ERROR;
    uint64_t totalBytesReceived = 0;
    bool networkError = false;
    bool connectionTimedOut = false;
    bool fileSaveError = false;

    while (true) {

        ssize_t bytesReceived = recv(socket, receivedBuffer.data(), receivedBuffer.size(), 0);

        if (bytesReceived < 0) {
            if (errno == EINTR) {
                continue;
            }

            networkError = true;
            connectionTimedOut = (errno == EAGAIN || errno == EWOULDBLOCK);
            break;
        }

        if (bytesReceived == 0) {

            break;
        }

        totalBytesReceived += (uint64_t)bytesReceived;
        receivedData.clear();
        crcChecker.feed(receivedBuffer.data(), (size_t)bytesReceived, receivedData);

        size_t dataStart = 0;

        if (!statusReceived && !receivedData.empty()) {
            responseStatus = (uint8_t)receivedData[0];
            statusReceived = true;
            dataStart = 1;

            if (responseStatus == STATUS_OK) {
                outputFile = fopen(outputFilePath, "wb");

                if (!outputFile) {
                    fileSaveError = true;
                    break;
                }
            }
        }

        if (receivedData.size() > dataStart) {
            if (responseStatus == STATUS_OK) {
                size_t dataSize = receivedData.size() - dataStart;

                if (fwrite(receivedData.data() + dataStart, 1, dataSize, outputFile) != dataSize) {
                    fileSaveError = true;
                    break;
                }
            }
            else {
                serverError.append(receivedData, dataStart, string::npos);
            }
        }
    }

    if (outputFile) {

        fclose(outputFile);
    }

    if (fileSaveError) {

        remove(outputFilePath);
         errorMessage = "cannot save the .tdbg file to disk";
        return false;
    }

    if (networkError) {
        remove(outputFilePath);

        if (connectionTimedOut) {
             errorMessage = "timed out waiting for the load balancer (the queue may be too long)";
        }
        else {
             errorMessage = "connection to the load balancer was lost";
        }

        return false;
    }

    if (totalBytesReceived == 0) {
         errorMessage = "no reply, the load balancer closed the connection (maybe we waited too long in its queu)";
        return false;
    }

    if (!crcChecker.verify()) {
        remove(outputFilePath);
         errorMessage = ERR_REPLY_CHECKSUM;
        return false;
    }

    if (!statusReceived) {
        remove(outputFilePath);
         errorMessage = "the server response did not contain a status";
        return false;
    }

    if (responseStatus != STATUS_OK) {
        errorMessage = trimEnd(serverError);

        if (errorMessage.empty()) {

             errorMessage = "the server reported an error without a message";
        }

        return false;
    }

    return  true;


    // Reply on the wire: [status][payload][CRC-32]. The CRC covers the status byte too.
    // Feed every received byte (status included) to a TrailerChecker.
    // Status 1: collect the payload into outError. Status 0: write the payload to the file.
    // At the end call verify(). If it fails, delete the file, set outError to
    // "checksum mismatch, response corrupted" and return false.
}


bool requestDebugFile(const char* host, int port, const char* sourcePath,
    const char* outPath, string& error) {


    FILE* file = fopen(sourcePath, "rb");

    if (!file) {
        error = string("cannot open source file: ") + sourcePath;
        return false;
    }
    fclose(file);
    int sock = connectToHost(host, port);
    if (sock < 0) {
        error = string("cannt connect to the load balancer at ") + host + ":" + to_string(port);
        return false;
    }
    timeval tv;
    tv.tv_sec = CLIENT_QUEUE_TIMEOUT_SEC;
    tv.tv_usec = 0;

    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    bool uploaded = uploadSource(sock, sourcePath);


    bool ok = receiveResponse(sock, outPath, error);

    close(sock);

    if (!ok && !uploaded && error == ERR_REPLY_CHECKSUM) {

        error = "couldnt send the source file to the load balancer";
    }



    return ok;
}

// =============================================================================================
// PART B: READING THE .tdbg FILE
// File layout (written by Phase 1):
//   header | snapshot 0 | snapshot 1 | ... | index (one 8 byte offset per snapshot)
// The index lets us jump straight to any step without reading the steps before it.
// =============================================================================================

bool readInt32(FILE* f, int32_t& value) {
    int x= fread(&value, sizeof(int32_t), 1, f);

    return  (x== 1);
}
bool readInt64(FILE* f, int64_t& value) {
    int x = fread(&value, sizeof(int64_t), 1, f);

    return  (x == 1);
}
bool readString(FILE* f, string& text){
    int32_t length;

    if (!readInt32(f, length)) {
        return false;

    }

    if (length < 0 || length > MAX_STRING_LENGTH) {
        return false;
    }

    text.resize((size_t)length);

    if (length > 0 && fread(&text[0],  1, (size_t)length,  f) != (size_t)length) {
        return false;
    }

    return true;
}
bool readVariable(FILE* file, Variable& var) {

    return readString(file, var.name) && readInt32(file, var.value);
}

bool readFrame(FILE* file, Frame&  fr){

    if (!readString(file,  fr.func_name)) {

        return false;

    }
    if (!readInt32( file, fr.argc)) {

        return false;
    }

    if (fr.argc < 0 || fr.argc > MAX_VARS_PER_FRAME) {

        return false;
    }

    for (int i = 0; i < fr.argc; i++) {
        if (!readVariable(file,  fr.argv[i])) {
            return false;
        }
    }
    if (!readInt32(file,  fr.returnLine)) {
        return false;
    }
    if (!readInt32(file, fr.localCount)) {
        return false;
    }



    if (fr.localCount < 0 || fr.localCount >  MAX_VARS_PER_FRAME) {
        return false;
    }

    for (int i = 0; i < fr.localCount; i++) {
        if (!readVariable(file,  fr.locals[i])) {
            return false;
        }
    }


    return true;


}

bool readSnapshotBody(FILE* file, Snapshot& snap) {

    if (!readInt32(file, snap.stackDepth)) {
        return false;
    }

    if (snap.stackDepth < 0 || snap.stackDepth > MAX_STACK_DEPTH) {
        return false;
    }

    for (int i = 0; i < snap.stackDepth; i++) {
        if (!readFrame(file, snap.callStack[i])) {
            return false;
        }
    }

    return true;
}


bool closeAndFail(FILE* file) {



    fclose(file);
    return false;
}


bool openTdbg(const char* path, TdbgReader& reader) {

    reader.file = nullptr;
    reader.index.clear();

    FILE* filee = fopen(path, "rb");

    if (!filee) {
        return false;
    }

    fseeko(filee, 0, SEEK_END);

    int64_t fileSzie = (int64_t)ftello(filee);

    fseeko(filee, 0, SEEK_SET);

    if (fileSzie < TTDB_HEADER_DISK_SIZE) {
        return closeAndFail(filee);
    }

    TTDBHeader haed;

    if (fread(haed.magic, 1, 4, filee) != 4 ||
        memcmp(haed.magic, "TTDB", 4) != 0) {
        return closeAndFail(filee);
    }

    if (!readInt32(filee, haed.version) ||
        !readInt32(filee, haed.stepCount) ||
        !readInt64(filee, haed.indexOffset)) {
        return closeAndFail(filee);
    }

    if (haed.version !=  TTDB_VERSION) {

        return closeAndFail(filee);
    }

    if (haed.stepCount <= 0) {

        return closeAndFail(filee);
    }

    if (haed.indexOffset < TTDB_HEADER_DISK_SIZE ||
        haed.indexOffset >  fileSzie) {

        return closeAndFail(filee);
    }

    if (haed.indexOffset + (int64_t)haed.stepCount * 8 != fileSzie) {

        return closeAndFail(filee);
    }

    vector<int64_t> indxx((size_t)haed.stepCount);

    fseeko(filee, (off_t)haed.indexOffset, SEEK_SET);

    for (int i = 0; i < haed.stepCount; i++) {

        if (!readInt64(filee, indxx[i])) {

            return  closeAndFail(filee);
        }

        bool vaildPos = (indxx[i] >= TTDB_HEADER_DISK_SIZE &&
            indxx[i] < haed.indexOffset);

        bool inOrdre = (i == 0 || indxx[i] > indxx[i - 1]);

        if (!vaildPos || !inOrdre) {

            return  closeAndFail(filee);
        }
    }

    Snapshot* snpa = new Snapshot;

    for (int i = 0; i < haed.stepCount;  i++) {

        int64_t endPso;

        if (i + 1 < haed.stepCount) {
            endPso = indxx[i + 1];
        }
        else {
            endPso = haed.indexOffset;
        }

        bool gdoo = (fseeko(filee, (off_t)indxx[i],  SEEK_SET) == 0)
            && readSnapshotBody(filee, *snpa)
            && ((int64_t)ftello(filee) == endPso);

        if (!gdoo) {
            delete snpa;
            return closeAndFail(filee);
        }
    }

    delete snpa;

    reader.file = filee;
    reader.header = haed;
    reader.index = indxx;

    return true;
}

void closeTdbg(TdbgReader& reader) {

    if (reader.file) {
        fclose(reader.file);
        reader.file = nullptr;
    }

    vector<int64_t>().swap(reader.index);


    // Free r.index and close the file.
}


Snapshot* readSnapshotAt(TdbgReader& r, int32_t step)

{
    if (!r.file || step < 0 || step >= r.header.stepCount) { 
        return nullptr;
    }

    if (fseeko(r.file, (off_t)r.index[step],  SEEK_SET) != 0) { 
        return nullptr;
    }

    Snapshot* snpa = new Snapshot;

    if (!readSnapshotBody(r.file, *snpa))


    {
        delete snpa;
        return nullptr;
    }
    return snpa;

}

// =============================================================================================
// PART C: THE SESSION IN MEMORY
// There is one DebugSession variable, created in main() and passed to every command by reference.
// =============================================================================================

void initSession(DebugSession& s)
{

    s.active_flag = 0;
    s.reader.file = nullptr;
    memset(&s.reader.header, 0, sizeof(s.reader.header));
    s.reader.index.clear();
    s.currentStep = 0;
}
void closeSession(DebugSession& s)
{
    if (s.active_flag){

        closeTdbg(s.reader);
    }
    s.active_flag = 0;
    // If active, call closeTdbg(s.reader). Then set active = false.
    // Called when the user types quit and when the input ends.
}

// =============================================================================================
// PART D: THE COMMANDS
// =============================================================================================

void printSnapshot(int32_t step, int32_t totalSteps, const Snapshot& s); // defined in Part E

bool showCurrent(DebugSession& s)
{

      Snapshot* snpa = readSnapshotAt(s.reader, (int32_t)s.currentStep);
        if (snpa == nullptr)
        {
            cout << "error: damaged .tdbg file" << endl;
            return false;
        }
        printSnapshot((int32_t)s.currentStep + 1, s.reader.header.stepCount, *snpa);   // +1: the user counts from 1
        delete snpa;
        return true;
    // Read the snapshot at s.currentStep, call printSnapshot, delete the snapshot.
    // If readSnapshotAt returns nullptr, print "damaged .tdbg file" and return false.
}
bool moveBy(DebugSession& s, int32_t detla)
{

    if (!s.active_flag){
    
        cout << "error: no session yet. Use: start <source file>" << endl;
        return false;
    }

    int64_t totl = s.reader.header.stepCount;
    int64_t targte = s.currentStep + detla;

    if (targte < 0 || targte >= totl)
    {
        if (detla == -1) { 

            cout << "already at the first step" << endl; 
        }
        else if (detla == 1) { 

            cout << "already at the last step" << endl;
        }
        else
        {
            cout << "cannot move there: step " << (targte + 1) << " does not exist (valid steps: 1 to " << totl << ")" << endl;
        }
        return false;
    }

    int64_t oldSetp = s.currentStep;
    s.currentStep = targte;

    if (!showCurrent(s)){

        s.currentStep = oldSetp;       
        return false;
    }
    return true;
}
bool cmdStart(DebugSession& s, const string& sourcePath)
{
    string tmpPaht = string(TDBG_TMP_FILE) + "." + to_string(getpid());
    string erro;
    if (!requestDebugFile(LB_HOST.c_str(), LB_PORT, sourcePath.c_str(), tmpPaht.c_str(), erro)) {
        cout << "error: " << erro << endl;
        return false;
    }

    TdbgReader frsh;

    if (!openTdbg(tmpPaht.c_str(), frsh)) {
        cout << "error: " << ERR_DAMAGED_TDBG << endl;
        remove(tmpPaht.c_str());
        return false;
    }

    if (s.active_flag) {

        closeTdbg(s.reader);
    }
    s.reader = frsh;
    s.currentStep = 0;
    s.active_flag = 1;

    rename(tmpPaht.c_str(), TDBG_FILE);
    return showCurrent(s);
    // 1. requestDebugFile with host and port, saving to TDBG_FILE.
    // 2. If it fails, print the error text and return false. Keep the old session as it was.
    // 3. Open the new file with openTdbg into a NEW local TdbgReader.
    //    If that fails, print "server sent a damaged .tdbg file", keep the old session, return false.
    // 4. Only now close the old reader (if s.active), then put the new reader into s,
    //    set s.currentStep = 0 and s.active = true.
    // 5. Show step 1 with showCurrent(s).
}
bool cmdNext(DebugSession& s) {
    return moveBy(s, 1);
}
bool cmdPrv(DebugSession& s) {

    return moveBy(s, -1);
}
bool cmdJmpNext(DebugSession& s, int32_t n) {
    return moveBy(s, n);
}
bool cmdJmpPrv(DebugSession& s, int32_t n) {
    return moveBy(s, -n);
}

// =============================================================================================
// PART E: PRINTING
// =============================================================================================

void printSnapshot(int32_t step, int32_t totalSteps, const Snapshot& s)
{

    cout << endl;

     cout << "Step " << step << " of " << totalSteps << " ===" << endl;
    cout << "Call stack, newest call first (depth " << s.stackDepth << "):" << endl;

    for (int32_t i = s.stackDepth - 1; i >= 0; i--) {
        const Frame& frmae =  s.callStack[i];

        cout << "  " <<  frmae.func_name << "(";
         
         for (int32_t j = 0; j < frmae.argc; j++) {
            if (j > 0) { 
                
                cout << ", ";
            }
            cout << frmae.argv[j].name << "=" << frmae.argv[j].value;
        }
        cout << ")" << endl;
        cout << "      locals: ";

         if (frmae.localCount == 0) {

            cout << "(none)";
        }
         for (int32_t j = 0; j < frmae.localCount; j++) {
            if (j > 0) { cout << ", "; }
            cout << frmae.locals[j].name << "=" << frmae.locals[j].value;
        }
        cout << endl;

         if (frmae.returnLine >= 0) {
            cout << "      return address: " << frmae.returnLine << endl;
        }
    }
}
void printHelp()
{
    cout << "Commands:" << endl;
    cout << "  start <source file>      send the program to the server and show step 1" << endl;
    cout << "  next                     go one step forward" << endl;
    cout << "  prv                      go one step back" << endl;
    cout << "  jmp next <n>             go n steps forward" << endl;
    cout << "  jmp prv <n>              go n steps back" << endl;
    cout << "  help                     show this text" << endl;
    cout << "  quit | exit              leave the debugger" << endl;
    cout << "(you can also type \"" << PROGRAM_NAME << "\" before a command, e.g. \"" << PROGRAM_NAME << " next\")" << endl;

    // Print the commands
}

// =============================================================================================
// PART F: THE INFINITE LOOP
// =============================================================================================

vector<string> splitWords(const string& line){

    istringstream in(line);
    vector<string> wrods;
    string wrod;

    while (in >> wrod) {

        wrods.push_back(wrod);
    }
    return wrods;
}

bool parsePositiveNumber(const string& text, int32_t& number)
{
    if (text.empty() || text.size() > 10) { 
        return false;
    }
    for (size_t i = 0; i < text.size(); i++) {
         if (text[i] < '0' || text[i] > '9') { 
            return false;
        }
    }
    long long vlaue = atoll(text.c_str());

    if (vlaue < 1 || vlaue > MAX_JUMP) { 
        return false;
    }
    number = (int32_t)vlaue;
    return true;
}


bool runCommand(DebugSession& s, const string& line, bool& quitRequested)
{
    vector<string> wrods = splitWords(line);
    if (wrods.empty()) {
        return  true;
    }

    if (wrods[0] == PROGRAM_NAME) {
        wrods.erase(wrods.begin());
        if (wrods.empty()) {
            printHelp();
            return  true;
        }
    }

    const string& cmd = wrods[0];

    if (cmd ==  "start" && wrods.size() == 2) {

        return cmdStart(s, wrods[1]);
    }
    if (cmd =="next" && wrods.size() == 1){
        return cmdNext(s);
    }
    if (cmd == "prv" && wrods.size() == 1){
        return cmdPrv(s);
    }
    if (cmd ==  "jmp" && wrods.size() == 3 &&  (wrods[1] == "next" || wrods[1] == "prv"))  {
        int32_t nmu;
        if (parsePositiveNumber(wrods[2], nmu)) {
            return (wrods[1] == "next") ? cmdJmpNext(s, nmu) : cmdJmpPrv(s, nmu);
        }
    }
    if (cmd == "help" && wrods.size() ==1) {
        printHelp();


        return true;
    }
    if ((cmd == "quit" || cmd == "exit") && wrods.size() == 1) {
        quitRequested = true;
        return true;
    }

    cout << "unknown command" << endl;
    printHelp();

    return false;

    // 1. Split the line into words.
    // 2. If the first word is "ttdb", drop it.
    // 3. Match the first word:
    //      start      -> needs a path after it, then cmdStart
    //      next       -> no more words, then cmdNext
    //      prv        -> no more words, then cmdPrv
    //      jmp        -> second word "next" or "prv", third word a positive number ,
    //                    then cmdJmpNext or cmdJmpPrv
    //      help       -> printHelp
    //      quit, exit -> set quitRequested = true
    // 4. For anything else, or a wrong number of words, print "unknown command" and the help text, and return false.
}
int main(int argc, char* argv[])
{
    if (argc >= 2) {
        LB_HOST =  argv[1];
    }
    if (argc >= 3) { 
        LB_PORT = atoi(argv[2]); 
    }
    if (argc > 3 || LB_PORT < 1 || LB_PORT > 65535){
        cerr << "usage: " << argv[0] << " <lb_ip> <lb_port>" << endl;

        return 1;
    }

    signal(SIGPIPE, SIG_IGN);
    DebugSession sesison;
    initSession(sesison);

    cout << "Time-Travel Debugger client. Load balancer: " << LB_HOST << ":" << LB_PORT << endl;
    printHelp();

    bool  quitRequseted = false;
    string lnie;
    while (true) {
        if (  quitRequseted) { 
            
            break;
        }

        cout << PROMPT << flush;
        if (! getline(cin, lnie)) {
            cout << endl;
            break;
        }

        runCommand(sesison, lnie, quitRequseted);
    }

    closeSession(sesison);


    return 0;
    // you get string LB_HOST and int LB_PORT = 0 in arguments of this main function. 

    // DebugSession session; initSession(session);
    // Print a short welcome and the help text.
    //
    // Infinite loop:
    //   if quitRequested, break
    //
    // After the loop: closeSession(session) and return 0.
    // Never exit the loop because of an error. Only quit or the end of the input leaves it.

}
