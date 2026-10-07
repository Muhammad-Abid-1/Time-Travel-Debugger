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
        Node *next;
    };
    Node *top;
    int32_t count;

public:
    // Implement these functions:
    Stack()
    { 
        top = nullptr;
        count = 0;
    }
    void push(const T &val)
    {
        if (count + 1 > MAX_STACK_DEPTH)
        {
            throw overflow_error("Stack overflow");
        }
        if (top == nullptr) {
            top = new Node();
            top->data = val;
            top->next = nullptr;
            count++;
        }
        else {
            Node* n = new Node();
            n->data = val;
            n->next = top;
            top = n;
            count++;
        }
    }
    T pop()
    {
        if (isEmpty()) {
            throw underflow_error("Stack underflow");
        }
        if (top->next == nullptr) {
            T result = top->data;
            delete top;
            top = nullptr;
            count--;
            return result;
        }
        T result = top->data;
        Node* temp = top;
        top = top->next;
        delete temp;
        count--;
        return result;
    }
    T &peek()
    {
        return top->data;
    }
    bool isEmpty()
    {
        if (top == nullptr)
            return true;
        return false;
    }
    int32_t depth()
    {
        return count;
    }
    int32_t snapshot_into(T out[], int32_t maxLen)
    {
        if (top == nullptr || maxLen <= 0){
            return 0;
        }
        Node* curr = top;
        int32_t written = 0;
        while (curr != nullptr && written < maxLen)
        {
            out[written] = curr->data;
            written++;
            curr = curr->next;
        }
        return written;
    }
};


// Timeline : doubly linked list of Snapshots
struct Snapshot; // fwd declaration;
struct TimelineNode
{
    Snapshot *data;
    TimelineNode *next;
    TimelineNode *prev;
};
class Timeline
{
    TimelineNode *head, *tail;
    int32_t stepCount;

public:
    // Implement these functions
    Timeline()
    {
        head = nullptr;
        tail = nullptr;
        stepCount = 0;
    }
    void record(Snapshot *s)
    {
        if (head == nullptr) {
            head = tail = new TimelineNode();
            head->data = s;
            head->prev = nullptr;
            tail->next = nullptr;
        }
        else {
            TimelineNode *t = new TimelineNode();
            t->data = s;
            t->next = nullptr;
            t->prev = tail;
            tail->next = t;
            tail = t;
        }
        stepCount++;
    }
    TimelineNode *begin()
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
void writeHeader(FILE *f, const TTDBHeader &h)
{
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
string trim(const string& str)
{
    int32_t start = 0;
    int32_t end = static_cast<int32_t>(str.length()) - 1;
    while (start <= end && (str[start] == ' ' || str[start] == '\t' || str[start] == '\r' || str[start] == '\n')){
        start++;
    }
    while (end >= start && (str[end] == ' ' || str[end] == '\t' || str[end] == '\r' || str[end] == '\n')){
        end--;
    }
    if (start > end) {
        return "";
    }
    return str.substr(start, end - start + 1);
}
bool readSourceLine(ifstream &in, string &out)
{
    string line;
    while (getline(in, line))
    {
        string trimmed = trim(line);
        if (!trimmed.empty())
        {
            out = trimmed;
            return true;
        }
    }

    out = "";
    return false;
}
string firstWord(const string &line)
{
    string f_word = "";
    int len = line.length();
    for (int32_t i = 0; i < len && line[i] != ' '; i++) {
        f_word += line[i];
    }
    return f_word;
}
string secondWord(const string &line)
{
    string s_word = "";
    int32_t len = static_cast<int32_t>(line.length());
    int32_t i = 0;
    while (i < len && line[i] != ' '){
        i++;
    }
    while (i < len && line[i] == ' ') {
        i++;
    }
    if (i >= len){
        return s_word;
    }
    while (i < len && line[i] != ' '){
        s_word += line[i];
        i++;
    }
    return s_word;
}
bool validateProgram(const char *sourcePath)
{
    ifstream file(sourcePath);
    if (!file.is_open()){
        throw runtime_error("Error: Could not open source file.");
    }
    string line;
    bool insideFunc = false;
    bool foundMain = false;
    int32_t lineNum = 0;
    while (readSourceLine(file, line)){
        lineNum++;
        string kw = firstWord(line);
        if (kw == "func"){
            if (insideFunc){
                throw runtime_error("Validation Error (Line " + to_string(lineNum) + "): Nested 'func' definitions are not allowed.");
            }
            insideFunc = true;
            if (secondWord(line) == "main"){
                foundMain = true;
            }
        }
        else if (kw == "func_end"){
            if (!insideFunc){
                throw runtime_error("Validation Error (Line " + to_string(lineNum) +  "): Unexpected 'func_end' outside of any function.");
            }
            insideFunc = false;
        }
    }
    if (insideFunc){
        throw runtime_error("Validation Error: Unclosed function definition (missing 'func_end').");
    }
    if (!foundMain){
        throw runtime_error("Validation Error: Missing 'main' function declaration.");
    }
    return true;
}

// PASS 0x1: RESOLVE() -> resolve.bin
int64_t writeResolveRecord(FILE *f, int64_t offsetField, const string &text)
{
    // writes one [offset(8B)][size(4B)][string] record at the current file position
    // returns this record's own starting byte position
}
int64_t readResolveRecord(FILE *f, string &outText)
{
    // reads one record at the current position and advances past it, returns the offset field - the raw line text comes back untouched in outText.
}
int64_t resolveProgram(const char *sourcePath, const char *resolveBinPath)
{
    FuncEntry funcArray[MAX_FUNCS];
    int32_t funcCount = 0;
    PendingPatch patches[MAX_PATCHES];
    int32_t patchCount = 0;
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
int32_t tokenizeLine(const string &line, Token tokens[], int32_t maxTokens)
{
    // first word is always a instruction keyword
    // instruction set = [func, func_end, call, set, add, sub, mul and div]
    // next word is identifier like name of a function, variable name
    // after identifier all are the params/arg, space separated
}
Snapshot *buildSnapshot(Stack<Frame> &callStack)
{
    // build the snapshot based on the callStack given
}
void executeProgram(const char *resolveBinPath, int64_t mainOffset, Timeline &timeline)
{
    // initialize the call stack
    // make the main frame
    // push main frame on the call stack

    // implementation:
    // execute line by line, and according to the keyword perform action
}

// PASS 0x3: SERIALIZE TIMELINE
void writeTdbg(Timeline &timeline, const char *tdbgPath)
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