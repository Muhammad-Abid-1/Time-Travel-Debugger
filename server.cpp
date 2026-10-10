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
/*#include <unistd.h>
#include <sys/socket.h>*/
#include <cstdint>
#include <cstdio>
#include<vector>
#include <sstream>
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

// ---- Custom data structures

// Stack: back the live Call Stack during execution
template <typename T>
class Stack
{
    struct Node
    {
        T data;
        Node* next;
        Node(const T& val, Node* n = nullptr) : data(val), next(n) {}
    };
    Node* top;
    int32_t count;

public:
    Stack() : top(nullptr), count(0) {}
    ~Stack()
    {
        while (!isEmpty()){
            pop();
        }
    }
    void push(const T& val)
    {
        if (count >= MAX_STACK_DEPTH){
           throw runtime_error( "Error: Stack overflow limit reached (" + MAX_STACK_DEPTH + ").");
            return;
        }
        top = new Node(val, top);
        count++;
    }
    T pop()
    {
        if (isEmpty()){
            throw runtime_error("Error: Stack underflow");
            return T();
        }
        Node* temp = top;
        T val = temp->data;
        top = top->next;
        delete temp;
        count--;
        return val;
    }
    T& peek()
    {
        if (isEmpty()){
            throw runtime_error("Error: Stack underflow");
        }
        return top->data;
    }
    bool isEmpty()
    {
        return top == nullptr;
    }

    int32_t depth()
    {
        return count;
    }

    int32_t snapshot_into(T out[], int32_t maxLen)
    {
        Node* curr = top;
        int32_t written = 0;
        while (curr != nullptr && written < maxLen){
            out[written++] = curr->data;
            curr = curr->next;
        }
        return written;
    }
};


// Timeline : doubly linked list of Snapshots
struct Snapshot; // fwd declaration;
struct TimelineNode
{
    Snapshot* data;
    TimelineNode* next;
    TimelineNode* prev;
    TimelineNode(Snapshot* s) : data(s), next(nullptr), prev(nullptr) {}
};

class Timeline
{
    TimelineNode* head, * tail;
    int32_t stepCount;

public:
    Timeline() : head(nullptr), tail(nullptr), stepCount(0) {}
    ~Timeline(){
        TimelineNode* curr = head;
        while (curr){
            TimelineNode* next = curr->next;
            delete curr->data;
            delete curr;
            curr = next;
        }
    }
    void record(Snapshot* s)
    {
        TimelineNode* node = new TimelineNode(s);
        if (!head){
            head = tail = node;
        }
        else{
            tail->next = node;
            node->prev = tail;
            tail = node;
        }
        stepCount++;
    }
    TimelineNode* begin()
    {
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
struct TTDBHeader
{
    char magic[4]; // "TTDB"
    int32_t version;
    int32_t stepCount;
    int64_t indexOffset;
};
void writeHeader(FILE* f, const TTDBHeader& h)
{
    fseek(f, 0, SEEK_SET);
    fwrite(h.magic, 1, 4, f);
    fwrite(&h.version, sizeof(int32_t), 1, f);
    fwrite(&h.stepCount, sizeof(int32_t), 1, f);
    fwrite(&h.indexOffset, sizeof(int64_t), 1, f);
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
    int64_t rawOffset = 0;
    if (!in.read(reinterpret_cast<char*>(&rawOffset), sizeof(int64_t))) 
        return false;
    uint32_t strSize = 0;
    if (!in.read(reinterpret_cast<char*>(&strSize), sizeof(uint32_t))) 
        return false;
    vector<char> buf(strSize + 1, 0);
    if (!in.read(buf.data(), strSize)) 
        return false;
    out = string(buf.data(), strSize);
    return true;
}
string cleanToken(const string& s)
{
    string res = "";
    for (char c : s)
    {
        if (isalnum(static_cast<unsigned char>(c)) || c == '_')
        {
            res += c;
        }
    }
    return res;
}
string firstWord(const string& line)
{
    stringstream ss(line);
    string w;
    if (ss >> w) 
        return cleanToken(w);
    return "";
}
string secondWord(const string& line)
{
    stringstream ss(line);
    string w1, w2;
    if (ss >> w1 >> w2) 
        return cleanToken(w2);
    return "";
}
bool validateProgram(const char* sourcePath)
{
    ifstream in(sourcePath, ios::binary);
    if (!in.is_open()) 
        return false;
    string line;
    Stack<string> funcStack;
    while (readSourceLine(in, line)){
        string kw = firstWord(line);
        if (kw == "func"){
            if (!funcStack.isEmpty()){
                return false;
            }
            funcStack.push(line);
        }
        else if (kw == "func_end"){
            if (funcStack.isEmpty()){
                return false;
            }
            funcStack.pop();
        }
    }
    return funcStack.isEmpty();
}

// PASS 0x1: RESOLVE() -> resolve.bin
int64_t writeResolveRecord(FILE* f, int64_t offsetField, const string& text)
{
    int64_t startPos = _ftelli64(f);
    uint32_t strSize = static_cast<uint32_t>(text.length());
    fwrite(&offsetField, sizeof(int64_t), 1, f);
    fwrite(&strSize, sizeof(uint32_t), 1, f);
    fwrite(text.c_str(), 1, strSize, f);
    return startPos;
}
int64_t readResolveRecord(FILE* f, string& outText)
{
    int64_t offsetField = 0;
    if (fread(&offsetField, sizeof(int64_t), 1, f) != 1) 
        return -1;
    uint32_t strSize = 0;
    if (fread(&strSize, sizeof(uint32_t), 1, f) != 1) 
        return -1;
    vector<char> buf(strSize + 1, 0);
    if (fread(buf.data(), 1, strSize, f) != strSize) 
        return -1;
    outText = string(buf.data(), strSize);
    return offsetField;
}
int64_t resolveProgram(const char* sourcePath, const char* resolveBinPath)
{
    ifstream in(sourcePath, ios::binary);
    if (!in.is_open()) 
        return -1;
    FILE* fRes = fopen(resolveBinPath, "wb+");
    if (!fRes) 
        return -1;
    FuncEntry funcArray[MAX_FUNCS];
    int32_t funcCount = 0;
    PendingPatch patches[MAX_PATCHES];
    int32_t patchCount = 0;
    string line;
    int64_t mainOffset = -1;
    while (readSourceLine(in, line)){
        string kw = firstWord(line);
        int64_t recordStartPos = _ftelli64(fRes);
        if (kw == "func"){
            string fName = secondWord(line);
            funcArray[funcCount++] = { fName, recordStartPos };
            if (fName == "main"){
                mainOffset = recordStartPos;
            }
            writeResolveRecord(fRes, recordStartPos, line);
        }
        else if (kw == "call"){
            string targetFunc = secondWord(line);
            patches[patchCount++] = { recordStartPos, targetFunc };
            writeResolveRecord(fRes, 0, line);
        }
        else{
            writeResolveRecord(fRes, recordStartPos, line);
        }
    }
    if (mainOffset == -1){
        fclose(fRes);
        return -1;
    }

    for (int i = 0; i < patchCount; i++){
        int64_t targetOffset = -1;
        for (int j = 0; j < funcCount; j++){
            if (funcArray[j].funcName == patches[i].targetFuncName){
                targetOffset = funcArray[j].byteOffsetInResolveBin;
                break;
            }
        }
        if (targetOffset == -1){
            fclose(fRes);
            return -1;
        }
        _fseeki64(fRes, patches[i].byteOffsetOfOffsetField, SEEK_SET);
        fwrite(&targetOffset, sizeof(int64_t), 1, fRes);
    }
    fclose(fRes);
    return mainOffset;
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
int32_t main()
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