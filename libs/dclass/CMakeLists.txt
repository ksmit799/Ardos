# WARNING: Don't override this CMake file with the Panda3D one.
# This one has been specially written for use with Ardos.

# NOTE: .prebuilt's have been stripped from file names (we're not running yak/flex)

# Source files should be kept up-to-date with the Panda3D dcparser.

target_compile_definitions(ardos PRIVATE YY_NO_UNISTD_H)

target_sources(ardos PUBLIC
        dcAtomicField.h dcAtomicField.I
        dcClass.h dcClass.I
        dcDeclaration.h
        dcField.h dcField.I
        dcFile.h dcFile.I
        dcKeyword.h dcKeywordList.h
        dcLexer.cxx dcLexerDefs.h
        dcMolecularField.h
        dcParser.h dcParserDefs.h
        dcParser.cxx
        dcSubatomicType.h
        dcPackData.h dcPackData.I
        dcPacker.h dcPacker.I
        dcPackerCatalog.h dcPackerCatalog.I
        dcPackerInterface.h dcPackerInterface.I
        dcParameter.h
        dcClassParameter.h
        dcArrayParameter.h
        dcSimpleParameter.h
        dcSwitchParameter.h
        dcNumericRange.h dcNumericRange.I
        dcSwitch.h
        dcTypedef.h
        dcbase.h
        dcindent.h
        dcmsgtypes.h
        hashGenerator.h
        primeNumberGenerator.h
        dcAtomicField.cxx
        dcClass.cxx
        dcDeclaration.cxx
        dcField.cxx
        dcFile.cxx
        dcKeyword.cxx
        dcKeywordList.cxx
        dcMolecularField.cxx
        dcSubatomicType.cxx
        dcPackData.cxx
        dcPacker.cxx
        dcPackerCatalog.cxx
        dcPackerInterface.cxx
        dcParameter.cxx
        dcClassParameter.cxx
        dcArrayParameter.cxx
        dcSimpleParameter.cxx
        dcSwitchParameter.cxx
        dcSwitch.cxx
        dcTypedef.cxx
        dcindent.cxx
        hashGenerator.cxx
        primeNumberGenerator.cxx
        )
