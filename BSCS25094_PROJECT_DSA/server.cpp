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
#include <cstdlib>
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
const int32_t MAX_STEPS = 5000;

// ---- Custom data structures

// Stack: back the live Call Stack during execution
template <typename T>
class Stack // it is implemented as singly linked list, where the next is pointing to the node below it
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
    ~Stack() {
        while (top != nullptr) {
            Node* old = top;
            top = top->next;
            delete old;

        }
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
    ~Timeline();
    void record(Snapshot* s)
    {
        TimelineNode* x = new TimelineNode;
        x->prev = tail;
        x->data = s;
        x->next = nullptr;

        if (tail != nullptr) {
            tail->next = x;
        }
        else {

            head = x;
        }
            tail = x;
            stepCount++;

        
       

        // add record in the timeline
    }
    TimelineNode* begin(){
        return head;
    
    }
    int32_t getStepCount()
    {
        return stepCount;
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

Timeline::~Timeline() {
    TimelineNode* cur = head;

    while (cur != nullptr) {
        TimelineNode* nextNode = cur->next;
        delete cur->data;
        delete cur;
        cur = nextNode;
    }
}
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
    fwrite(&h.stepCount, sizeof(int32_t), 1, f);
    fwrite(&h.indexOffset, sizeof(int64_t), 1, f);

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
    size_t e = line.find_first_of(" \t", s);
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
    size_t e = line.find_first_of(" \t", s);
    if (e == string::npos) {
        return "";
    }
    s = line.find_first_not_of(" \t", e);
    if (s == string::npos) {
        return "";
    }
    e = line.find_first_of(" \t", s);
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
    outText.resize(size);
    if (size > 0 && fread(&outText[0], 1, size, f) != (size_t)size) {
        return END_OF_FILE;
    }
    return offsetField;
    // reads one record at the current position and advances past it, returns the offset field - the raw line text comes back untouched in outText.
}
int64_t findFunc(FuncEntry funcs[], int32_t ct, const string& name) {
    for (int32_t i = 0; i < ct; i++) {
        if (funcs[i].funcName == name) {


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
        return -1;

    }
    FILE* out = fopen(resolveBinPath, "w+b");
    if (!out) {
        cerr << "File cant be created :(" << endl;
        return -1;
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
    }

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
    int32_t ct = 0;
    size_t pos = 0;
    while (ct < maxTokens) {
        size_t start = line.find_first_not_of(" \t", pos);
        if (start == string::npos) {
            break;
        }
        size_t end = line.find_first_of(" \t", start);
        if (end == string::npos) {
            end = line.size();

        }
        tokens[ct].text = line.substr(start, end - start);
        if (ct == 0) {
           tokens[ct].type = KEYWORD;

        }


        else if (ct == 1) {
            tokens[ct].type = IDENTIFIER;
        }
        else {
            tokens[ct].type = PARAM;

        }
        ct++;
        pos = end;
    }
    return ct;

    // first word is always a instruction keyword
    // instruction set = [func, func_end, call, set, add, sub, mul and div]
    // next word is identifier like name of a function, variable name
    // after identifier all are the params/arg, space separated
}
void makeFrame(Frame &f, Token tokens[], int32_t t_num_tokens, int32_t returnLine) {
    f.func_name = tokens[1].text;

    f.argc = t_num_tokens - 2;
    for (int32_t i = 0; i < f.argc; i++) {
         f.argv[i].name = tokens[2 + i].text;
        f.argv[i].value = 0;
    }
    f.returnLine = returnLine;
    f.localCount = 0;
}

Variable* findVar(Frame& f, const string& n) {
    for (int32_t i = 0; i < f.argc; i++) {
        if (f.argv[i].name == n) {
            return &f.argv[i];

        }
    }
    for (int32_t i = 0; i < f.localCount; i++) {
        if (f.locals[i].name == n) {
            return &f.locals[i];
        }
    }
    return nullptr;
}
bool isNum(const string& n) {
    if (n.empty()) {
        return false;
    }
    size_t x = 0;
        if (n[0] == '-') {
            x = 1;
        }
    if ( x>= n.size()) {
        return false;
    }
    for (size_t j = x; j < n.size(); j++) {
        if (n[j] < '0' || n[j] > '9') {
            return false;
        }
    }
    return true;
}
bool getVal(Frame& f, const string& text, int32_t& res) {
    if (isNum(text)) {
        res = stoi(text);
        return true;
    }
    Variable* v = findVar(f, text);
    if (v == nullptr) {
        cerr << "error" << endl;
        return false;
    }
    res = v->value;
    return true;


}
bool set_var(Frame& f, const string& n, int32_t val) {

    Variable* v= findVar(f, n);
    if (v == nullptr) {
        if (f.localCount >= MAX_VARS_PER_FRAME) {
            cerr << "error. too many vars in a functon: " << f.func_name <<endl;

            return false;
        }
        v = &f.locals[f.localCount];
        v->name = n;
        f.localCount++;
    }
    v->value = val;
    return true;
}
Snapshot* buildSnapshot(Stack<Frame>& callStack)
{
    Snapshot* what_da_snap = new Snapshot();
    what_da_snap->stackDepth = callStack.snapshot_into(what_da_snap->callStack, MAX_STACK_DEPTH);
    
    return what_da_snap;
    // build the snapshot based on the callStack given
}
bool executeProgram(const char* resolveBinPath, int64_t mainOffset, Timeline& timeline)
{

    FILE* f = fopen(resolveBinPath, "rb");
    if (!f) {
        cerr << "error. can't open the file: " << resolveBinPath << endl;
        return false;
    }


    Stack<Frame> callStack;
    string line;
    Token tokens[MAX_TOKENS];

    //make the main frame and push it on the call stack

    fseek(f, (long)mainOffset, SEEK_SET);
    readResolveRecord(f, line);
    int32_t n = tokenizeLine(line, tokens, MAX_TOKENS);

    Frame mainFrame{};
    makeFrame(mainFrame, tokens, n, -1); // -1 if nobody called the main
    callStack.push(mainFrame);
    timeline.record(buildSnapshot(callStack));

    while (!callStack.isEmpty()) {

        if (timeline.getStepCount() >= MAX_STEPS) {
            cerr << "error. the program runs for too many steps." << endl;
            fclose(f);
            return false;
        }

        int64_t recordStart = ftell(f);
        int64_t field = readResolveRecord(f, line);
        if (field == END_OF_FILE) {
            cerr << "error. ran past the end of the program." << endl;
            fclose(f);
            return false;
        }
        n = tokenizeLine(line, tokens, MAX_TOKENS);

        string keyword = tokens[0].text;
        Frame& current = callStack.peek();
        bool success = true;

        if (keyword == "set") {
            int32_t value = 0;
            if (n < 3) {
                cerr << "error. set needs a name and a value" << endl;
                success = false;
            }
            else if (!getVal(current, tokens[2].text, value)) {
                success = false;
            }
            else if (!set_var(current, tokens[1].text, value)) {
                success = false;
            }
        }
        else if (keyword == "add" || keyword == "sub" || keyword == "mul" || keyword == "div") {
            Variable* target = nullptr;
            int32_t other = 0;
            if (n < 3) {
                cerr << "error. " << keyword << " needs two operands" << endl;
                success = false;
            }
            else {
                target = findVar(current, tokens[1].text);
                if (target == nullptr) {
                    cerr << "error: variable: " << tokens[1].text << " is not defined" << endl;
                    success = false;
                }
                else if (!getVal(current, tokens[2].text, other)) {
                    success = false;
                }
            }
            if (success) {
                int64_t a = target->value;
                int64_t b = other;
                int64_t r = 0;

                if (keyword == "add") {
                    r = a + b;
                }
                else if (keyword == "sub") {
                    r = a - b;
                }
                else if (keyword == "mul") {
                    r = a * b;
                }
                else {
                    if (b == 0) {
                        cerr << "Oops. division by zero not possible" << endl;
                        success = false;
                    }
                    else {
                        r = a / b;
                    }
                }

                if (success && (r > INT32_MAX || r < INT32_MIN)) {
                    cerr << "error. the result does not fit in 32 bits" << endl;
                    success = false;
                }
                if (success) {
                    target->value = (int32_t)r;
                }
            }
        }   
        else if (keyword == "call") {
            int32_t argCount = n - 2;
            int32_t argValues[MAX_VARS_PER_FRAME];

            if (n < 2) {
                cerr << "error call needs a function name" << endl;
                success = false;
            }
            else if (callStack.depth() >= MAX_STACK_DEPTH) {
                cerr << "error. stack overflow (too many nested calls)" << endl;
                success = false;
            }
            else {
                for (int32_t i = 0; i < argCount && success; i++) {
                    if (!getVal(current, tokens[2 + i].text, argValues[i])) {
                        success = false;
                    }
                }
            }
            if (success) {
                string headerLine;
                Token headerTokens[MAX_TOKENS];
                fseek(f, (long)field, SEEK_SET);
                readResolveRecord(f, headerLine);
                int32_t num_tokens = tokenizeLine(headerLine, headerTokens, MAX_TOKENS);

                if (num_tokens - 2 != argCount) {
                    cerr << "error: " << headerTokens[1].text << " expects " << (num_tokens - 2) << " argument but got " << argCount << endl;
                    success = false;
                }
                else {
                    Frame newFrame{};
                    makeFrame(newFrame, headerTokens, num_tokens, (int32_t)recordStart);

                    for (int32_t i = 0; i < argCount; i++) {
                        newFrame.argv[i].value = argValues[i];
                    }
                    callStack.push(newFrame);
                }
            }
        }
        else if (keyword == "func_end") {
            Frame finished = callStack.pop();
            if (finished.returnLine >= 0) { // main has -1, nothing to return to
                string callLine;
                Token callTokens[MAX_TOKENS];
                fseek(f, (long)finished.returnLine, SEEK_SET);
                readResolveRecord(f, callLine);
                tokenizeLine(callLine, callTokens, MAX_TOKENS);

                Frame& caller = callStack.peek();
                for (int32_t i = 0; i < finished.argc; i++) {
                    Variable* v = findVar(caller, callTokens[2 + i].text);
                    if (v != nullptr) {
                        v->value = finished.argv[i].value;
                    }
                }
            }
        }
        else {
            cerr << "error. unknown instruction: " << keyword << endl;
            success = false;
        }

        if (!success) {
            cerr << "  (stopped at: " << line << ")" << endl;
            fclose(f);
            return false;
        }
        timeline.record(buildSnapshot(callStack)); //1 snapshot after evry line
    }

    fclose(f);
    return true;

    // initialize the call stack
    // make the main frame
    // push main frame on the call stack

    // implementation:
    // execute line by line, and according to the keyword perform action
}
void printTimeline(Timeline& timeline){
    int32_t st = 0;


    for (TimelineNode* node = timeline.begin(); node != nullptr; node = node->next){

        Snapshot* s = node->data;
        cout << "Step " << st << " (depth " << s->stackDepth << ")" << endl;

        for (int32_t i = 0; i < s->stackDepth; i++){
            Frame& fr = s->callStack[i];
            cout << "   " << fr.func_name << ": ";

            for (int32_t j = 0; j < fr.argc; j++) {
                cout << fr.argv[j].name << "=" << fr.argv[j].value << " ";
            }


            for (int32_t j = 0; j < fr.localCount; j++) {
                cout << fr.locals[j].name << "=" << fr.locals[j].value << " ";
            }

            cout << endl;

        }
        st++;
    }
}

// PASS 0x3: SERIALIZE TIMELINE
void writeInt32(FILE* f, int32_t value){
    fwrite(&value, sizeof(int32_t), 1, f);
}
void writeString(FILE* f, const string& s){
    writeInt32(f, (int32_t)s.size());
    fwrite(s.c_str(), 1, s.size(), f);
}
void writeVariable(FILE* f, const Variable& v){
    writeString(f, v.name);
    writeInt32(f, v.value);
}

void writeFrame(FILE* f, const Frame& fr) {

    writeString(f, fr.func_name);
    writeInt32(f, fr.argc);
    for (int32_t i = 0; i < fr.argc; i++) { 
        writeVariable(f, fr.argv[i]);
    }
    writeInt32(f, fr.returnLine);
    writeInt32(f, fr.localCount);
    for (int32_t i = 0; i < fr.localCount; i++) { 
        writeVariable(f, fr.locals[i]);
    }
}

void writeSnapshot(FILE* f, const Snapshot& s){

    writeInt32(f, s.stackDepth);


    for (int32_t i = 0; i < s.stackDepth; i++) {
        writeFrame(f, s.callStack[i]);
    }
}

bool writeTdbg(Timeline& timeline, const char* tdbgPath)
{
    FILE* f = fopen(tdbgPath, "w+b");
    if (!f) {
        cerr << "unable to create file: " << tdbgPath << endl;
        return false;
    }
    TTDBHeader h;

    h.magic[0] = 'T';
    h.magic[1] = 'T';
    h.magic[2] = 'D';
    h.magic[3] = 'B';

    h.version = 1;
    h.stepCount = 0; //num of snapshots in the timeline
    h.indexOffset = 0; // file position where the snapshot index begind
    writeHeader(f, h);

    int32_t count = timeline.getStepCount();
    int64_t* index = new int64_t[count];
    int32_t i = 0;
    for (TimelineNode* node = timeline.begin(); node != nullptr; node = node->next){

        index[i] = ftell(f);
        writeSnapshot(f, *node->data);
        i++;
    }
    h.indexOffset = ftell(f);
    for (int32_t k = 0; k < count; k++) {
        fwrite(&index[k], sizeof(int64_t), 1, f);
    }

    h.stepCount = count;
    fseek(f, 0, SEEK_SET);
    writeHeader(f, h);

    delete[] index;
    fclose(f);
    return true;

    // placeholder for header
    // index array of the size of stepcount from the timeline
    // placing each snapshot in the file while maintaining the index(starting point of each nth snapshot)
    // after timeline add the index array i the file
    // update the header
}

int32_t readInt32(FILE* f){
    int32_t value = 0;

    if (fread(&value, sizeof(int32_t), 1, f) != 1) {
        return 0;
    }
    return value;
}

string readString(FILE* f){
    int32_t length = readInt32(f);

    if (length < 0 || length > 1000000) { //broken file
        return "";
    }
    string s;

    s.resize(length);

    if (length > 0 && fread(&s[0], 1, length, f) != (size_t)length) {


        return "";
    }
    return s;
}

void showSnapshotFromTdbg(const char* tdbgPath, int32_t step){
    FILE* f = fopen(tdbgPath, "rb");

    if (!f){
        cerr << "error. cannot open: " << tdbgPath << endl;
        return;
    }

    char magic[4] = { 0, 0, 0, 0 };

    if (fread(magic, 1, 4, f) != 4 || magic[0] != 'T' || magic[1] != 'T' || magic[2] != 'D' || magic[3] != 'B'){
        cerr << "error. not a .tdbg file" << endl;


        fclose(f);
        return;
    }
    int32_t version = readInt32(f);
    int32_t stepCount = readInt32(f);
    int64_t indexOffset = 0;

    if (fread(&indexOffset, sizeof(int64_t), 1, f) != 1) {

        indexOffset = 0;
    }
    cout << "version " << version << ", " << stepCount << " steps, index at byte " << indexOffset << endl;

    if (step < 0 || step >= stepCount){
        cerr << "error. step " << step << " does not exist" << endl;
        fclose(f);

        return;
    }

    int64_t snapshotStart = 0;
    fseek(f, (long)(indexOffset + (int64_t)step * 8), SEEK_SET);

    if (fread(&snapshotStart, sizeof(int64_t), 1, f) != 1){
        fclose(f);
        return;
    }

    fseek(f, (long)snapshotStart, SEEK_SET);


    int32_t depth = readInt32(f);
    cout << "Step " << step << " (depth " << depth << ")" << endl;

    for (int32_t i = 0; i < depth; i++){
        cout << "   " << readString(f) << ": ";
        int32_t argc = readInt32(f);

        for (int32_t j = 0; j < argc; j++){
            string name = readString(f);
            cout << name << "=" << readInt32(f) << " ";
        }
        readInt32(f); 

        int32_t localCount = readInt32(f);

        for (int32_t j = 0; j < localCount; j++){
            string name = readString(f);
            cout << name << "=" << readInt32(f) << " ";
        }
        cout << endl;
    }
    fclose(f);
}
// main section
int32_t main(int argc, char* argv[])
{
    const char* path = (argc > 1) ? argv[1] : "source.bin";

    // Pass 0x0: validate
    if (!validateProgram(path)){
        return 1;
    }

    //Pass 0x1: resolve
    int64_t mainOffset = resolveProgram(path, "resolve.bin");
    if (mainOffset < 0){
        return 1;
    }

    // Pass 0x2: execute
    Timeline timeline;
    if (!executeProgram("resolve.bin", mainOffset, timeline)){
        return 1;
    }
    cout << "Executed " << timeline.getStepCount() << " steps" << endl;

    // Pass 0x3: serialize
    if (!writeTdbg(timeline, "session.tdbg")){
        return 1;

    }
    cout << "session.tdbg written" << endl;

    // optional test:  ./server source.bin 4 - reads step 4 back from the file

    if (argc > 2){
        showSnapshotFromTdbg("session.tdbg", atoi(argv[2]));
    }

    return 0;
}