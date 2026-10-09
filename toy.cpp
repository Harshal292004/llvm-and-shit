#include <string>
#include <memory>
#include <vector>
#include <map>
// Value is the fundamental base class for all data and operations computed by a program 
// Serves as the primary unit for static single assigment
// Acts as the superclass for critical componenets ( Function , Global Value , Constant )
// every computed result in LLVM IR is a Value
// All Values have a Type
// Type is not a subclass of Value
// SOme Values can have a name and they belong to some module 
// setting the name on the Value automatically updates the modules symbol table 
#include "llvm/IR/Value.h"
#include "llvm/IR/IRBuilder.h"
//
#include "llvm/IR/Module.h"
using namespace std;
// lexer emits tokens [0-255] if its an unkown character , otherwise one of these for known thing
enum Token {
    // end of file
    tok_eof=-1,
    // define keyword
    tok_def=-2,
    // extern keyword
    tok_extern=-3,
    // identifier like a variable
    tok_identifier=-4,
    // 64 bit floating point number
    tok_number=-5,
};

static string IdentifierStr; // Filled in if tok_identifier
static double NumVal; // filled in if tok_number
// + will be returned as its ascii value

static int gettok(){
    static int LastChar=' ';
    while(isspace(LastChar))
        LastChar = getchar();
    if (isalpha(LastChar)){
        IdentifierStr = LastChar;
        while (isalnum((LastChar=getchar())))
            IdentifierStr+=LastChar;
        if(IdentifierStr=="def")    
            return Token::tok_def;
        if(IdentifierStr=="extern")
            return Token::tok_extern;
        return Token::tok_identifier;
    }
    if(isdigit(LastChar) || LastChar=='.'){
        string NumStr;
        do{
            NumStr+=LastChar;
            LastChar=getchar();
        } while (isdigit(LastChar) || LastChar=='.');
        
        NumVal= strtod(NumStr.c_str(),0);
        return Token::tok_number;
    }

    if (LastChar== '#'){
        do 
            LastChar = getchar();
        while( LastChar !=EOF  && LastChar!= '\n' && LastChar !='\r');
        if(LastChar !=EOF)
            return gettok();
    }

    if(LastChar==EOF)
        return Token::tok_eof;
    int ThisChar= LastChar;
    LastChar= getchar();
    return ThisChar;
}

static int CurTok;

static int getNextToken(){
    return CurTok=gettok();
}

// parser
class ExprAST{
public:
    virtual ~ExprAST()=default;
    // code gen returns a pointer Value*
    // codegen method says to emit IR for the AST node along with all the things it depends on 
    // Values represent a SSA register or a SSA value in LLVM , Static Single Assignment register
    virtual  llvm::Value* codegen()=0;
};

class NumberExprAST :  public ExprAST{
    double Val;    
public:
    NumberExprAST(double val): Val(val){}
    llvm::Value *codegen() override{
        // ConstanFP holds numberic value in an APFloat ( Arbitatry Precision Float)
        return llvm::ConstantFP::get(*TheContext,llvm::APFloat(Val));
    }
};

class VariableExprAST: public ExprAST{
    string Name;
public:
    VariableExprAST(const string &Name):Name(Name){}
    llvm::Value *codegen () override{
         // Look this variable up in the function.
        llvm::Value *V = NamedValues[Name];
        if (!V)
            LogErrorV("Unknown variable name");
        return V;
    }
};
// unique_ptr can't be coppied becuase it represents EXCLUSIVE OWNERSHIP
// we move the ownership of unique ptr from a to b after using a 
// now a is pointing to null where as b points to the resource aquired by the unique_ptr
// unique_ptr is basically a pointer  that owns an object and automatically deletes it when the owner dies 
class BinaryExprAST: public ExprAST{
    char Op;
    unique_ptr<ExprAST> LHS,RHS;
    public:
    BinaryExprAST(char Op,unique_ptr<ExprAST> LHS,
    unique_ptr<ExprAST> RHS):
    Op(Op),LHS(move(LHS)),RHS(move(RHS)){}

    llvm::Value* codegen() override{
        // generate code for both operands 
        // LHS & RHS both create the IR using recurrsion
        llvm::Value *L= LHS->codegen();
        llvm::Value *R= RHS->codegen();
        // if either child fails generation return nnullptr indicating failure 
        if(!L || !R)
            return nullptr;
        
        switch (Op)
        {
            case '+':
                // creates LLVM floating point addition 
                // %addtmp= fadd double %L,%R
                // Fadd used as the value are double (  floating point doubles )
                // addtmp is the name of the generated insturction 
                return Builder->CreateFAdd(L,R,"addtmp");
            case '-':
                // creates LLVM fp substraction
                // %subtmp=fsub double %L , %R
                return Builder->CreateFSub(L,R,"subtmp");
            case '*':
                // %multmp = fmul double %L, %R
                return Builder->CreateFMul(L,R,"multmp");
            case '<':
                // FCmp -> Floating point comparison
                // U -> UNordered comparison
                // LT -> Less Than 
                // with special behaciour for NaN values as is unordered comparison
                L= Builder->CreateFCmpULT(L,R,"cmptmp");
                // Convertes the interger style ( i1 ) commparison to double comparision as i1 true => double 1.0
                //i1 false => double 0.0 
                // making it easier for comaprsion 1.0 < 2.0
                // Create Unsigned Integer To Floating Point
                return Builder->CreateUIToFP(L,llvm::Type::getDoubleTy(*TheContext),"booltmp");
            default:
                return LogErrorV("invalid binary operator");
        }

    }
};

class CallExprAST : public ExprAST{
    string Callee;
    // the vector class it self has a pointer to the memory allocated for array objects me move that to new location 
    // we cant use Args(Args) as assigment copies every elemnt of the array to new one and copy on unique_ptr is not allowed
    vector<unique_ptr<ExprAST>> Args;
    public:
    CallExprAST(const string &Callee,
        vector<unique_ptr<ExprAST>> Args) : Callee(Callee), Args(move(Args)){}

    llvm::Value* codegen() override{
        llvm::Function  *CalleeF= TheModule->getFunction(Callee);
        if(!CalleeF)
            return LogErrorV("Incorrect # arguments passed");
        vector<llvm::Value*> ArgsV;
        for(unsigned i=0 , e=Args.size();i!=e;i++){
            // Args[i] can be any NumberExpr , IdetifierExpr and accordingly will run code gen method 
            ArgsV.push_back(Args[i]->codegen());
            if(!ArgsV.back())
                return nullptr;

        }
        return Builder->CreateCall(CalleeF,ArgsV,"calltmp");
    }
};

class PrototypeAST{
    string Name;
    vector<string> Args;

    public:
    PrototypeAST(const string &Name, vector<string> Args): Name(Name), Args(move(Args)){}
    // returns a refrence to a string and the caller cant modify that string through this refrence
    // the const at getName() const  means the member function promises not to modify the PrototypeAST object. 
    const string& getName() const { return Name; }


    llvm::Function* codegen(){
        vector<llvm::Type*> DOubles(Args.size(),llvm::Type::getDoubleTy(*TheContext));

        llvm::FunctionType *FT= llvm::FunctionType::get(llvm::Type::getDoubleTy(*TheContext),DOubles,false);
        llvm::Function *F=llvm::Function::Create(FT,llvm::Function::ExternalLinkage,Name,TheModule.get());
        unsigned Idx=0;
        for(auto &Arg:F->args())
            Arg.setName(Args[Idx++]);
        return F;
    }
};

class FunctionAST{
    unique_ptr<PrototypeAST> Proto;
    unique_ptr<ExprAST> Body;
    public:
    FunctionAST(unique_ptr<PrototypeAST> Proto, unique_ptr<ExprAST> Body) : Proto(move(Proto)),Body(move(Body)){}
    llvm::Function* codegen(){
        llvm::Function *TheFunction = TheModule->getFunction(Proto->getName());
        if(!TheFunction)
     {
               TheFunction = Proto->codegen();
            return nullptr;
    }  
    if(!TheFunction->empty()){
        return(llvm::Function*) LogErrorV("function cant be redefined");
    }
}
};


// for x+y we need 
// unique_ptr<ExprAST> LHS=  make_unique<VariableExprAST>("x");
// unique_ptr<ExprAST> RHS=  make_unique<VariableExprAST>("y");
// unique_ptr<BinaryExprAST> Result=  make_unique<BinaryExprAST>('+',move(LHS),move(RHS));


// LogError* - Error handlers for Parsing
unique_ptr<ExprAST> LogError(const char *str){
    fprintf(stderr,"Error: %s\n",str);
    return nullptr;
}
unique_ptr<PrototypeAST> LogErrorP(const char *str){
    LogError(str);
    return nullptr;
}


// LLVMContext is owner of the LLVM0wide uniquing tables and chaces for one IR universe 
// its not a  general continaer for all IR obkjects 
// Tables/data structurres associated with LLCM Context exist mainly for uniquing , interning and sharing immmutatble IR obkects 
// type uniquing tbales 
// IntergerType , pointerType , FunctionType , etc ( Ensures equivalent types are shared rather than recreated 
// Constant Uniquing  => ConstantInt , ConstatnFP => Same costant can be reused throught a module 
// Metadata uniquing  
//
// Say :
// LLVMContext C;
// Type *A = Type::getInt32Ty(C);
// Type *B = Type::getInt32Ty(C);
// You dont get 2 independent i32 types 
static unique_ptr<llvm::LLVMContext> TheContext;
static unique_ptr<llvm::IRBuilder<>> Builder;
static  unique_ptr<llvm::Module> TheModule;
// this shit acts like symbol table 
static map<string ,llvm::Value *> NamedValues;

// Error logs during code gen
llvm::Value * LogErrorV(const char *str){
    LogError(str);
    return nullptr;
}


// Parser

// numberexpr ::= number
static unique_ptr<ExprAST> ParseNumberExpr(){
    auto Result= make_unique<NumberExprAST> (NumVal);
    getNextToken();
    return move(Result);
}

// parenexpr ::= '(' expression ')'
static unique_ptr<ExprAST> ParseParenExpr(){
    getNextToken();// eat (
    // for something like (x>3)
    // after eating ( x>3 will be parsed by the Parse Expression
    auto V= ParseExpression();
    if(!V)
        return nullptr;
    // here the current token is expected to be )
    if(CurTok!=')')
        return LogError("Expected ')'");
    getNextToken(); // eat )
    return V;
}

// identifiererexpr
// ::= identifier
// ::= identifier '('
static unique_ptr<ExprAST> ParseIdentifierExpr(){
    string IdName=IdentifierStr;
    getNextToken(); // eat identifier
    // Simple variable refrence rather than a function declaration of call
    if(CurTok != '(')
        return make_unique<VariableExprAST>(IdName);
    getNextToken();  // eat (
    vector<unique_ptr<ExprAST>> Args;
    if(CurTok != ')'){
        while (true)
        {
            if(auto Arg= ParseExpression())
                Args.push_back(move(Arg));
            else
                return nullptr;
            if(CurTok==')')
                break;
            if(CurTok!=',') 
                return LogError("Expected ')' or ',' in argument list");
            getNextToken(); // eat , 
        }
    }
    getNextToken();// eat ')'
    return make_unique<CallExprAST>(IdName,move(Args));
}
static unique_ptr<ExprAST>  ParsePrimary(){
    switch(CurTok){
        case tok_identifier:
            return ParseIdentifierExpr();
        case tok_number:
            return ParseNumberExpr();
        case '(':
            return ParseParenExpr();
        default:
            return LogError("unkown token when expecting an expression");
    }
}
/// @brief Binary RHS parser
///        ::= ('+' primary)*
/// @param ExprPrec int
/// @param LHS unique_pointer<ExprAST>
/// @return  unique_pointer<ExprAST>
static unique_ptr<ExprAST> ParseBinOpRHS(int ExprPrec,unique_ptr<ExprAST> LHS){
    while (true)
    {
        int TokPrec = GetTokPrecedence();
        if(TokPrec < ExprPrec)
            return LHS;
        int BinOp=CurTok;
        getNextToken(); // eat binary Operation
        auto RHS=ParsePrimary();
        if(!RHS)
            return nullptr;
        int NextPrec = GetTokPrecedence();
        if(TokPrec < NextPrec){
            RHS= ParseBinOpRHS(TokPrec+1,move(RHS));
            if(!RHS)
                return nullptr;
        }
        // Merge LHS/RHS in BinaryExprAST node in AST.
        LHS=make_unique<BinaryExprAST>(BinOp, move(LHS),move(RHS));
    }
}
static unique_ptr<ExprAST> ParseExpression(){
  auto LHS=ParsePrimary();
  if(!LHS)
    return nullptr;
  return ParseBinOpRHS(0,move(LHS));
}

// prototype 
// ::= id '(' id* ')'
// the grammer states the prototype to be iden(x y z)
// rather than iden(x,y,z) x y z is handled by the function through white space skip in gettok()
static unique_ptr<PrototypeAST> ParsePrototype(){
    if(CurTok != Token::tok_identifier)
        return LogErrorP("Expected fucntion name in prototype");
    string FnName=IdentifierStr;
    getNextToken();
    if(CurTok!='(')
        return LogErrorP("Expected '(' in prototype");
    // Read list of arguments
    vector<string> ArgNames;
    while(getNextToken()==tok_identifier)
        ArgNames.push_back(IdentifierStr);
    if(CurTok!=')')
        return LogErrorP("Expected ')' in prototype ");
    getNextToken();
    //For an AST node, you're constructing the object and intend for it to own its arguments. Therefore:
    // move(ArgNames) rather than refrencing argNames with vector<string> &Args
    return make_unique<PrototypeAST>(FnName, move(ArgNames));
}

// difference between prototype and definition is that definition is basically a function signature  where as functon definition are the expression written inside a fucntion 
// here we consider only a single function experession
static unique_ptr<FunctionAST> ParseDefinition(){
    // eat def 
    getNextToken();
    // get function signature
    auto Proto= ParsePrototype();
    if(!Proto) return nullptr;
    if (auto E= ParseExpression()){
        return make_unique<FunctionAST>(move(Proto),move(E));
    } 
    return nullptr;
}

static unique_ptr<PrototypeAST> ParseExtern(){
    getNextToken();
    return ParsePrototype();
}

// toplevelexpr ::= expression
// for top levels expressiosn with no fucntion body or definetion like 4+5 
//FunctionAST
//├── PrototypeAST("__anon_expr", [])
//└── Expression
//  └── 4 + 5
//
static unique_ptr<FunctionAST> ParseTopLevelExpr(){
    if( auto E=ParseExpression()){
       auto Proto= make_unique<PrototypeAST>("__anon_expr",vector<string>()); 
       return make_unique<FunctionAST>(move(Proto),move(E)); 
    }
    return nullptr;
}

// table of precedence for handling binary expressions
static map<char,int> BinoPrecendence= {
    {'<',10},
    {'+',20},
    {'-',20},
    {'*',40} 
};

static int GetTokPrecedence(){
    if(!isascii(CurTok))
        return -1;
    int TokPrec=BinoPrecendence[CurTok];
    if(TokPrec <=0 ) return -1;
    return TokPrec;
}


static void HandleDefinition(){
    if(ParseDefinition()){
        fprintf(stderr, "Parsed a function definition.\n");
    }else{
        // skip token for error recovery
        getNextToken();
    }
}

static void HandleExtern(){
    if(ParseExtern()){
        fprintf(stderr, "Parsed a extern definition.\n");     
    }else{
        getNextToken();
    }
}


static void HandleTopLevelExpression(){
    if(ParseTopLevelExpr()){
        fprintf(stderr,"Parsed a tld");
    }else{
        getNextToken();
    }
}


// MainLoop everything before ; is the top level expression 
static void MainLoop(){
    while (true)
    {
        fprintf(stderr,"ready> ");
        switch (CurTok)
        {
        case Token::tok_eof:    
            return;
        case ';': // tld finished
            getNextToken();
            break;
        case Token::tok_extern:
            HandleExtern();
            break;
        case Token::tok_def:
            HandleDefinition();
            break;
        default:
            HandleTopLevelExpression();
            break;
        }
    }
}

int main(){
  // Prime the first token.
  fprintf(stderr, "ready> ");
  getNextToken();

  // Run the main "interpreter loop" now.
  MainLoop();
  return 0;
}
