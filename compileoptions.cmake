# Disable frame pointer optimizations so profilers can get better call stacks
add_compile_options(-fno-omit-frame-pointer)

# The -fms-extensions enable the stuff like __if_exists, __declspec(uuid()), etc.
add_compile_options(-fms-extensions )
#-fms-compatibility      Enable full Microsoft Visual C++ compatibility
#-fms-extensions         Accept some non-standard constructs supported by the Microsoft compiler

# Make signed arithmetic overflow of addition, subtraction, and multiplication wrap around
# using twos-complement representation (this is normally undefined according to the C++ spec).
add_compile_options(-fwrapv)

add_definitions(-DDISABLE_CONTRACTS)
# The -ferror-limit is helpful during the porting, it makes sure the compiler doesn't stop
# after hitting just about 20 errors.
add_compile_options(-ferror-limit=4096)

# All warnings that are not explicitly disabled are reported as errors
add_compile_options(-Werror)

# Disabled warnings
add_compile_options(-Wno-unused-private-field)
add_compile_options(-Wno-unused-variable)
# Explicit constructor calls are not supported by clang (this->ClassName::ClassName())
add_compile_options(-Wno-microsoft)
# This warning is caused by comparing 'this' to NULL
add_compile_options(-Wno-tautological-compare)
# There are constants of type BOOL used in a condition. But BOOL is defined as int
# and so the compiler thinks that there is a mistake.
add_compile_options(-Wno-constant-logical-operand)

add_compile_options(-Wno-unknown-warning-option)

#These seem to indicate real issues
add_compile_options(-Wno-invalid-offsetof)
# The following warning indicates that an attribute __attribute__((__ms_struct__)) was applied
# to a struct or a class that has virtual members or a base class. In that case, clang
# may not generate the same object layout as MSVC.
add_compile_options(-Wno-incompatible-ms-struct)

# Some architectures (e.g., ARM) assume char type is unsigned while CoreCLR assumes char is signed
# as x64 does. It has been causing issues in ARM (https://github.com/dotnet/coreclr/issues/4746)
add_compile_options(-fsigned-char)

if(CLR_CMAKE_PLATFORM_UNIX_ARM)
   # Because we don't use CMAKE_C_COMPILER/CMAKE_CXX_COMPILER to use clang
   # we have to set the triple by adding a compiler argument
   add_compile_options(-mthumb)
   add_compile_options(-mfpu=vfpv3)
   if(ARM_SOFTFP)
     add_definitions(-DARM_SOFTFP)
     add_compile_options(-mfloat-abi=softfp)
     add_compile_options(-target armv7-linux-gnueabi)
   else()
     add_compile_options(-target armv7-linux-gnueabihf)
   endif(ARM_SOFTFP)
endif(CLR_CMAKE_PLATFORM_UNIX_ARM)
