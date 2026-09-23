struct C {};
struct Base { Base(int); };
struct D : Base {
  D(int C) : Base(C) {}
  int f(int x) { return x; }
};
C after_member_parameter;
int g(int C) { return C; }
C after_function_parameter;
auto lambda = [](int C) { return C; };
C after_lambda_parameter;
struct Holder { typedef int hidden_type; };
void outside_class_scope() { hidden_type * pointer; }
