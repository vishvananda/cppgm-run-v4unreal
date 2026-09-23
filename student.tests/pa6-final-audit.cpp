// PA6 audit reducer: enum constant rules and x86-64 type-forming layout.
enum class Width { zero = 0, one = 1, two = one + 1, three = Width::two + 1 };
static_assert(static_cast<int>(Width::three) == 3, "prior scoped enumerators");
constexpr Width enum_constant = Width::one;
constexpr Width explicit_enum_constant = static_cast<Width>(2);
constexpr int integral_constant = static_cast<int>(Width::two);
constexpr int constant_base = 5;
typedef const int& ConstIntReference;
ConstIntReference constant_reference = constant_base;
int reference_constant_bound[constant_reference];
static_assert(!(0 && (1 / 0)), "the unselected operand is not evaluated");
static_assert(1 || (1 / 0), "the unselected operand is not evaluated");

struct ReferenceMember { int &value; };
struct Base { int value; };
struct Derived : Base { long extra; };
struct VirtualBase { virtual void f(); };
struct VirtualDerived : VirtualBase { int value; };
struct StaticMember { static int value; int payload; };
int StaticMember::value;
decltype(nullptr) null_value;
int null_size[sizeof(decltype(nullptr))];
int null_align[alignof(decltype(nullptr))];
static_assert(sizeof(decltype(nullptr)) == 8, "x86-64 nullptr_t size");
static_assert(alignof(decltype(nullptr)) == 8, "x86-64 nullptr_t alignment");
static_assert(sizeof(int &) == 4, "sizeof reference type uses referent size");
static_assert(sizeof(ReferenceMember) == 8, "x86-64 reference data member slot");
static_assert(alignof(ReferenceMember) == 8, "x86-64 reference data member alignment");
static_assert(sizeof(Derived) == 16, "single non-virtual base layout");
static_assert(sizeof(VirtualBase) == 8, "x86-64 vptr layout");
static_assert(sizeof(VirtualDerived) == 16, "inherited vptr layout");
static_assert(sizeof(StaticMember) == 4, "out-of-class static definition is not a subobject");

struct Hidden { int member; };
void hidden_type_lookup() {
  int Hidden;
  struct Hidden *pointer;
}
namespace Qualification { typedef char Value; }
namespace Nested {
  struct Qualification { typedef int Value; };
  Qualification::Value selected_value;
}
void adjusted_parameter(int source_array[3]);
void adjusted_parameter(int *source_pointer);
int same_parameters_different_return_type(int);
double separate_function_different_return_type(int);
