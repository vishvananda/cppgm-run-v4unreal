namespace catalog {
inline namespace v1 {
typedef const int Value;
int consume(Value input);
}
}

using catalog::Value;
constexpr int extent = 1 || (1 / 0);
typedef Value Grid[extent + 2];
enum class Choice { width = 3 };
static_assert(Choice::width == Choice::width, "same scoped enum");
namespace chain_0 { typedef int TLeaf; }
namespace chain_1 { using namespace chain_0; }
namespace chain_2 { using namespace chain_1; }
namespace chain_3 { using namespace chain_2; }
namespace chain_4 { using namespace chain_3; }
namespace chain_5 { using namespace chain_4; }
namespace chain_6 { using namespace chain_5; }
namespace chain_7 { using namespace chain_6; }
namespace chain_8 { using namespace chain_7; }
namespace chain_9 { using namespace chain_8; }
namespace chain_10 { using namespace chain_9; }
namespace chain_11 { using namespace chain_10; }
namespace chain_12 { using namespace chain_11; }
namespace chain_13 { using namespace chain_12; }
namespace chain_14 { using namespace chain_13; }
namespace chain_15 { using namespace chain_14; }
namespace chain_16 { using namespace chain_15; }
namespace chain_17 { using namespace chain_16; }
namespace chain_18 { using namespace chain_17; }
namespace chain_19 { using namespace chain_18; }
namespace chain_20 { using namespace chain_19; }
namespace chain_21 { using namespace chain_20; }
namespace chain_22 { using namespace chain_21; }
namespace chain_23 { using namespace chain_22; }
namespace chain_24 { using namespace chain_23; }
namespace chain_25 { using namespace chain_24; }
namespace chain_26 { using namespace chain_25; }
namespace chain_27 { using namespace chain_26; }
namespace chain_28 { using namespace chain_27; }
namespace chain_29 { using namespace chain_28; }
namespace chain_30 { using namespace chain_29; }
namespace chain_31 { using namespace chain_30; }
namespace chain_32 { using namespace chain_31; }
namespace chain_33 { using namespace chain_32; }
namespace chain_34 { using namespace chain_33; }
namespace chain_35 { using namespace chain_34; }
namespace chain_36 { using namespace chain_35; }
namespace chain_37 { using namespace chain_36; }
namespace chain_38 { using namespace chain_37; }
namespace chain_39 { using namespace chain_38; }
namespace chain_40 { using namespace chain_39; }
using namespace chain_40;
TLeaf deep_value;
Grid values;
struct Object { char tag; long int value; };
int size_values[sizeof(Object)];
int align_values[alignof(Object)];
static_assert(sizeof(Object) == 16, "x86-64 aggregate layout");
static_assert(alignof(Object) == 8, "x86-64 aggregate alignment");
typedef int& IntReference;
int ref_size[sizeof(IntReference)];
static_assert(sizeof(IntReference) == 4, "reference sizeof uses referred type");
int (*callback)(int samples[4]);
void inspect() {
  Grid local;
  { Choice current; }
}
