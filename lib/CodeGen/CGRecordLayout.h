//===--- CGRecordLayout.h - LLVM Record Layout Information ------*- C++ -*-===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#ifndef CLANG_CODEGEN_CGRECORDLAYOUT_H
#define CLANG_CODEGEN_CGRECORDLAYOUT_H

#include "clang/AST/CharUnits.h"
#include "clang/AST/Decl.h"
#include "clang/Basic/LLVM.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/DerivedTypes.h"

namespace llvm {
  class StructType;
}

namespace clang {
namespace CodeGen {

/// \brief Structure with information about how a bitfield should be accessed.
///
/// Often we layout a sequence of bitfields as a contiguous sequence of bits.
/// When the AST record layout does this, we represent it in the LLVM IR's type
/// as either a sequence of i8 members or a byte array to reserve the number of
/// bytes touched without forcing any particular alignment beyond the basic
/// character alignment.
///
/// Then accessing a particular bitfield involves converting this byte array
/// into a single integer of that size (i24 or i40 -- may not be power-of-two
/// size), loading it, and shifting and masking to extract the particular
/// subsequence of bits which make up that particular bitfield. This structure
/// encodes the information used to construct the extraction code sequences.
/// The CGRecordLayout also has a field index which encodes which byte-sequence
/// this bitfield falls within. Let's assume the following C struct:
///
///   struct S {
///     char a, b, c;
///     unsigned bits : 3;
///     unsigned more_bits : 4;
///     unsigned still_more_bits : 7;
///   };
///
/// This will end up as the following LLVM type. The first array is the
/// bitfield, and the second is the padding out to a 4-byte alignmnet.
///
///   %t = type { i8, i8, i8, i8, i8, [3 x i8] }
///
/// When generating code to access more_bits, we'll generate something
/// essentially like this:
///
///   define i32 @foo(%t* %base) {
///     %0 = gep %t* %base, i32 0, i32 3
///     %2 = load i8* %1
///     %3 = lshr i8 %2, 3
///     %4 = and i8 %3, 15
///     %5 = zext i8 %4 to i32
///     ret i32 %i
///   }
///
struct CGBitFieldInfo {
  /// The offset within a contiguous run of bitfields that are represented as
  /// a single "field" within the LLVM struct type. This offset is in bits.
  unsigned Offset : 16;

  /// The total size of the bit-field, in bits.
  unsigned Size : 15;

  /// Whether the bit-field is signed.
  unsigned IsSigned : 1;

  /// The storage size in bits which should be used when accessing this
  /// bitfield.
  unsigned StorageSize;

  /// The alignment which should be used when accessing the bitfield.
  unsigned StorageAlignment;

  CGBitFieldInfo()
      : Offset(), Size(), IsSigned(), StorageSize(), StorageAlignment() {}

  CGBitFieldInfo(unsigned Offset, unsigned Size, bool IsSigned,
                 unsigned StorageSize, unsigned StorageAlignment)
      : Offset(Offset), Size(Size), IsSigned(IsSigned),
        StorageSize(StorageSize), StorageAlignment(StorageAlignment) {}

  void print(raw_ostream &OS) const;
  void dump() const;

  /// \brief Given a bit-field decl, build an appropriate helper object for
  /// accessing that field (which is expected to have the given offset and
  /// size).
  static CGBitFieldInfo MakeInfo(class CodeGenTypes &Types,
                                 const FieldDecl *FD,
                                 uint64_t Offset, uint64_t Size,
                                 uint64_t StorageSize,
                                 uint64_t StorageAlignment);
};

/// CGRecordLayout - This class handles struct and union layout info while
/// lowering AST types to LLVM types.
///
/// These layout objects are only created on demand as IR generation requires.
class CGRecordLayout {
  friend class CodeGenTypes;

  CGRecordLayout(const CGRecordLayout &) LLVM_DELETED_FUNCTION;
  void operator=(const CGRecordLayout &) LLVM_DELETED_FUNCTION;

private:
  /// The LLVM type corresponding to this record layout; used when
  /// laying it out as a complete object.
  llvm::StructType *CompleteObjectType;

  /// The LLVM type for the non-virtual part of this record layout;
  /// used when laying it out as a base subobject.
  llvm::StructType *BaseSubobjectType;

  /// Map from (non-bit-field) struct field to the corresponding llvm struct
  /// type field no. This info is populated by record builder.
  llvm::DenseMap<const FieldDecl *, unsigned> FieldInfo;

  /// Map from (bit-field) struct field to the corresponding llvm struct type
  /// field no. This info is populated by record builder.
  llvm::DenseMap<const FieldDecl *, CGBitFieldInfo> BitFields;

  // FIXME: Maybe we could use a CXXBaseSpecifier as the key and use a single
  // map for both virtual and non virtual bases.
  llvm::DenseMap<const CXXRecordDecl *, unsigned> NonVirtualBases;

  /// Map from virtual bases to their field index in the complete object.
  llvm::DenseMap<const CXXRecordDecl *, unsigned> CompleteObjectVirtualBases;

  /// False if any direct or indirect subobject of this class, when
  /// considered as a complete object, requires a non-zero bitpattern
  /// when zero-initialized.
  bool IsZeroInitializable : 1;

  /// False if any direct or indirect subobject of this class, when
  /// considered as a base subobject, requires a non-zero bitpattern
  /// when zero-initialized.
  bool IsZeroInitializableAsBase : 1;

public:
  CGRecordLayout(llvm::StructType *CompleteObjectType,
                 llvm::StructType *BaseSubobjectType,
                 bool IsZeroInitializable,
                 bool IsZeroInitializableAsBase)
    : CompleteObjectType(CompleteObjectType),
      BaseSubobjectType(BaseSubobjectType),
      IsZeroInitializable(IsZeroInitializable),
      IsZeroInitializableAsBase(IsZeroInitializableAsBase) {}

  /// \brief Return the "complete object" LLVM type associated with
  /// this record.
  llvm::StructType *getLLVMType() const {
    return CompleteObjectType;
  }

  /// \brief Return the "base subobject" LLVM type associated with
  /// this record.
  llvm::StructType *getBaseSubobjectLLVMType() const {
    return BaseSubobjectType;
  }

  /// \brief Check whether this struct can be C++ zero-initialized
  /// with a zeroinitializer.
  bool isZeroInitializable() const {
    return IsZeroInitializable;
  }

  /// \brief Check whether this struct can be C++ zero-initialized
  /// with a zeroinitializer when considered as a base subobject.
  bool isZeroInitializableAsBase() const {
    return IsZeroInitializableAsBase;
  }

  /// \brief Return llvm::StructType element number that corresponds to the
  /// field FD.
  unsigned getLLVMFieldNo(const FieldDecl *FD) const {
    assert(FieldInfo.count(FD) && "Invalid field for record!");
    return FieldInfo.lookup(FD);
  }

  unsigned getNonVirtualBaseLLVMFieldNo(const CXXRecordDecl *RD) const {
    assert(NonVirtualBases.count(RD) && "Invalid non-virtual base!");
    return NonVirtualBases.lookup(RD);
  }

  /// \brief Return the LLVM field index corresponding to the given
  /// virtual base.  Only valid when operating on the complete object.
  unsigned getVirtualBaseIndex(const CXXRecordDecl *base) const {
    assert(CompleteObjectVirtualBases.count(base) && "Invalid virtual base!");
    return CompleteObjectVirtualBases.lookup(base);
  }

  /// \brief Return the BitFieldInfo that corresponds to the field FD.
  const CGBitFieldInfo &getBitFieldInfo(const FieldDecl *FD) const {
    assert(FD->isBitField() && "Invalid call for non bit-field decl!");
    llvm::DenseMap<const FieldDecl *, CGBitFieldInfo>::const_iterator
      it = BitFields.find(FD);
    assert(it != BitFields.end() && "Unable to find bitfield info");
    return it->second;
  }

  void print(raw_ostream &OS) const;
  void dump() const;
};

}  // end namespace CodeGen
}  // end namespace clang

#endif
