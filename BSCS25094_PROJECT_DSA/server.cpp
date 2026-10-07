// ======================= TIME-TRAVEL DEBUGGER - SERVER TEMPLATE =======================

// Pipeline this file implements, top to bottom:
//   0. Receive  -- stream the client's .trace bytes straight to source.bin on disk
//   1. Pass 0X0   -- validity check (FUNC/FUNC_END matching)
//   2. Pass 0X1   -- resolve(): copy EVERY source line into resolve.bin as [offset][size][string], then patch CALL targets.
//   3. Pass 0X2   -- execute resolve.bin: tokenize ONE line at a time, update the call stack, take a snapshot -> Timeline
//   4. Pass 0X3   -- serialize Timeline -> session.tdbg(header + snapshot records + dense index)


#include <iostream>
#include <string>
#include <cstdint>
#include <fstream>
#include <unistd.h>
#include <sys/socket.h>
#include <cstdint>
#include <cstdio>
using namespace std;

// ---- Constants ----
const int32_t MAX_VARS_PER_FRAME = 16;
const int32_t MAX_STACK_DEPTH = 64;
const int32_t MAX_FUNCS = 128;
const int32_t MAX_TOKENS = MAX_VARS_PER_FRAME + 2; // kW + func_name + upto 16 params/args
const int32_t MAX_PATCHES = MAX_FUNCS * 4;
const uint64_t MAX_SOURCE_BYTES = 15ULL * 1024 * 1024; // sanity cap on the declared file length
const int32_t IO_BUFFER_SIZE = 64 * 1024;                  // fixed buffer for streaming to/from disk
const int32_t SOCKET_TIMEOUT_SEC = 5;                      // TODO: apply as SO_RCVTIMEO so a deadclient can't hang the server forever
const int64_t NOT_PATCHED = -1;
const int64_t END_OF_FILE = -2;

// ---- Custom data structures

// Stack: back the live Call Stack during execution
template <typename T>
class Stack
{
    struct Node
    {
        T data;
        Node* next;
    };
    Node* top;
    int32_t count;

public:
    // Implement these functions:
    Stack()
    { 
        top = nullptr;
        count = 0;
        // initialize the stack
    }
    void push(const T& val)
    {
        if (count >= MAX_STACK_DEPTH) {
            return;
        }
        Node* n = new Node;

        n->data = val;
        n->next = top; // pointing at the prev top
        top = n;

        count++;

    }
    T pop()
    {
        if (top == nullptr) {

            return T();
        }
        Node* old = top;
        T value = old->data;
        top = old->next;
        delete old;
        count--;
        return value;
    }
    T& peek()
    {
        return top->data;
        // returns the top value on the stack
    }
    bool isEmpty()
    {
        return count == 0;

    }
    int32_t depth()
    {
        return count;

    }
    int32_t snapshot_into(T out[], int32_t maxLen)
    {
        int32_t x = 0;
        Node* cur = top;
        while (cur != nullptr && x < maxLen)
        {
            out[x] = cur->data; // out[0] = top frame (the running function)
            x++;
            cur = cur->next;
        }
        return x;
        // copies every frame, top to bottom in the array given as a parameter
        // this is what buildSnapshot() call, returns count written
    }
};


// Timeline : doubly linked list of Snapshots
struct Snapshot; // fwd declaration;
struct TimelineNode
{
    Snapshot* data;
    TimelineNode* next;
    TimelineNode* prev;
};
class Timeline
{
    TimelineNode* head, * tail;
    int32_t stepCount;

public:
    // Implement these functions
    Timeline()
    {
        head = nullptr;
        tail = nullptr;
        stepCount = 0;
    }
    void record(Snapshot* s)
    {
        // add record in the timeline
    }
    TimelineNode* begin()
        return head;
    {
    }
    int32_t getStepCount()
    {
        return head;
    }
};

// Core structs
struct Variable
{
    string name;
    int32_t value;
};
struct Frame
{
    string func_name;
    int32_t argc;
    Variable argv[MAX_VARS_PER_FRAME];
    int32_t returnLine;
    Variable locals[MAX_VARS_PER_FRAME];
    int32_t localCount;
};
struct Snapshot
{
    Frame callStack[MAX_STACK_DEPTH];
    int32_t stackDepth;
};
struct TTDBHeader
{
    char magic[4]; // "TTDB"
    int32_t version;
    int32_t stepCount;
    int64_t indexOffset;
};
void writeHeader(FILE* f, const TTDBHeader& h)
{
    fwrite(h.magic, 1, 4, f);
    fwrite(&h.version, sizeof(int32_t), 1, f);

    // placeholder for other two data members
}

// resolve.bin - bookkeeping
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



// PASS 0x0: READING source.bin + VALIDITY CHECK
bool readSourceLine(ifstream& in, string& out)
{
    string l;
    while (getline(in, l)) {
        if (!l.empty() && l.back() == '\r') {
            l.pop_back();
        }
        if (l.find_first_not_of(" \t") == string::npos) {
            continue;
        }
        // reads the next nonblank line
        out = l;
        return true;
    }
    return false;


}
string firstWord(const string& line)
{
    size_t s = line.find_first_not_of(" \t");
    if (s == string::npos) {
        return "";
    }
    size_t e = line.find_first_not_of(" \t", s);
    if (e == string::npos) {
        e = line.size();
    }
    return line.substr(s, e - s);

    // returns first word from the input string
}
string secondWord(const string& line)
{
    size_t s = line.find_first_not_of(" \t");
    if (s == string::npos) {
        return "";
    }
    size_t e = line.find_first_not_of(" \t", s);
    if (e == string::npos) {
        return "";
    }
    s = line.find_first_not_of(" \t", e);
    if (s == string::npos) {
        return "";
    }
    e = line.find_first_not_of(" \t", s);
    if (e == string::npos) {
        e = line.size();
    }
    return line.substr(s, e - s);

    // returns the second word
}
bool validateProgram(const char* sourcePath) {
    ifstream in(sourcePath);
    if (!in) {
        cerr << "Error! can't open " << sourcePath << endl;
        return false;
    }
    bool inDaFunc = false;
    int32_t lineNum = 0;
    string line;
    while (readSourceLine(in, line)) {
        lineNum++;

        string f_wor = firstWord(line);
        if (f_wor == "func") {
            if (secondWord(line).empty()) {
                cerr << "Error in line# " << lineNum << " ): func has no name :(" << endl;
                return false;
            }
            if (inDaFunc) {
                cerr << "Error (line " << lineNum << " ): nested func declaration :(" << endl;
                return false;
            }
            inDaFunc = true;

        }
        else if (f_wor == "func_end") {
            if (!inDaFunc) {
                cerr << "Error (line " << lineNum << " ): func_end without matching func :(" << endl;
                return false;
            }
            inDaFunc = false;
        }
    }
    if (inDaFunc) {
        cerr << "Error: func is missing its func_end" << endl;

        return false;
    }

    return true;
    // for each func defined there should be exactly one func_end and no nested funcs allowed - 
}

// PASS 0x1: RESOLVE() -> resolve.bin
int64_t writeResolveRecord(FILE* f, int64_t offsetField, const string& text)
{
    int64_t mypos = ftell(f);
    int32_t size = (int32_t)text.size();
    fwrite(&offsetField, sizeof(int64_t), 1, f);
    fwrite(&size, sizeof(int32_t), 1, f);

    fwrite(text.c_str(), 1, size, f);

    return mypos;
    // writes one [offset(8B)][size(4B)][string] record at the current file position
    // returns this record's own starting byte position
}
int64_t readResolveRecord(FILE* f, string& outText)
{
    int64_t offsetField = 0;
    
    int32_t size = 0;
    if (fread(&offsetField, sizeof(int64_t), 1, f) != 1) {
        return END_OF_FILE;
    }
    if (fread(&size, sizeof(int32_t), 1, f) != 1) {
        return END_OF_FILE;

    }
    outtext.resize(size);
    if (size > 0 && fread(&outText[0], 1, size, f) != (size_t)size) {
        return END_OF_FILE;
    }
    return offsetField;
    // reads one record at the current position and advances past it, returns the offset field - the raw line text comes back untouched in outText.
}
int64_t findFunc(FuncEntry funcs[], int32_t ct, const string& name) {
    for (int32_t i = 0; i < ct; i++) {
        if (func[i].funcName == name) {


            return funcs[i].byteOffsetInResolveBin;
        }
    }
    return -1;
}
int64_t resolveProgram(const char* sourcePath, const char* resolveBinPath)
{
    FuncEntry funcArray[MAX_FUNCS];
    int32_t funcCount = 0;
    PendingPatch patches[MAX_PATCHES];
    int32_t patchCount = 0;
    ifstream in(sourcePath);
    if (!in) {
        cerr << "File not found :(" << endl;

    }
    FILE* out = fopen(resolveBinPath, "w+b");
    if (!out) {
        cerr << "File cant be created :(" << endl;
    }
    int64_t curr_offset = 0;
    string line;
    while (readSourceLine(in, line)) {
        string word = firstWord(line);
        int64_t offsetField = curr_offset;
        if (word == "func") {
            if (funcCount >= MAX_FUNCS) {
                cerr << "too many functions" << endl;
                fclose(out);
                return -1;
            }
            funcArray[funcCount].funcName = secondWord(line);
            funcArray[funcCount].byteOffsetInResolveBin = curr_offset;
            funcCount++;
        }
        else if (word == "call") {
            if (patchCount >= MAX_PATCHES) {
                cerr << "too many patches" << endl;
                fclose(out);
                return -1;
            }
            patches[patchCount].byteOffsetOfOffsetField = curr_offset;
            patches[patchCount].targetFuncName = secondWord(line);
            patchCount++;
            offsetField = NOT_PATCHED;
        }
        writeResolveRecord(out, offsetField, line);
        curr_offset = curr_offset + 8 + 4 + (int64_t)line.size();

        int64_t mainOffset = findFunc(funcArray, funcCount, "main");
        if (mainOffset < 0){
        
            cerr << "there's no main function" << endl;
            fclose(out);
            return -1;
        }
        for (int32_t i = 0; i < patchCount; i++) {
            int64_t target = findFunc(funcArray, funcCount, patches[i].targetFuncName);
            if (target < 0){
                cerr << "Call to undefined function: " << patches[i].targetFuncName  << endl;
                fclose(out);
                return -1;
            }
            fseek(out, (long)patches[i].byteOffsetOfOffsetField, SEEK_SET); // go back
            fwrite(&target, sizeof(int64_t), 1, out);
        }


    }
    fclose(out);
    return mainOffset;

    // Every source line becomes one record holding the raw line, as-is.
    // resolve() only PEEKS at the leading word(s) -- enough to spot FUNC
    // (remember its position) and CALL (remember which function it needs
    // and where its offset field sits).
    // Once the whole file is written, every CALL's offset field is patched
    // with its target's position. Patching happens after the full write
    // Returns the byte offset of main's FUNC header record.
    // if there is no main return the error 
}

// PASS 0x2: EXECUTION (tokenization happens here)
enum TokenType
{
    KEYWORD,
    IDENTIFIER,
    PARAM
};
struct Token
{
    TokenType type;
    string text;
};
int32_t tokenizeLine(const string& line, Token tokens[], int32_t maxTokens)
{
    // first word is always a instruction keyword
    // instruction set = [func, func_end, call, set, add, sub, mul and div]
    // next word is identifier like name of a function, variable name
    // after identifier all are the params/arg, space separated
}
Snapshot* buildSnapshot(Stack<Frame>& callStack)
{
    // build the snapshot based on the callStack given
}
void executeProgram(const char* resolveBinPath, int64_t mainOffset, Timeline& timeline)
{
    // initialize the call stack
    // make the main frame
    // push main frame on the call stack

    // implementation:
    // execute line by line, and according to the keyword perform action
}

// PASS 0x3: SERIALIZE TIMELINE
void writeTdbg(Timeline& timeline, const char* tdbgPath)
{
    // placeholder for header
    // index array of the size of stepcount from the timeline
    // placing each snapshot in the file while maintaining the index(starting point of each nth snapshot)
    // after timeline add the index array i the file
    // update the header
}
// main section
int32_t main2()
{

    if (!validateProgram("source.bin"))
    {
        // send an error response instead of a .tdbg file
        return 1;
    }

    int64_t mainOffset = resolveProgram("source.bin", "resolve.bin");

    Timeline timeline;
    executeProgram("resolve.bin", mainOffset, timeline);

    writeTdbg(timeline, "session.tdbg");

    return 0;
}
int32_t main()
{
    if (!validateProgram("source.bin"))
    {
        return 1;
    }
    cout << "source.bin is valid" << endl;
    return 0;
}