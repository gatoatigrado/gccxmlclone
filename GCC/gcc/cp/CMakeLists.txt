ADD_EXECUTABLE(cc1plus
  call.c decl.c expr.c pt.c typeck2.c class.c decl2.c error.c lex.c
  parse.c ptree.c rtti.c spew.c typeck.c cvt.c except.c friend.c init.c
  method.c search.c semantics.c tree.c repo.c dump.c optimize.c mangle.c
  cp-lang.c
  ../attribs.c ../c-common.c ../c-format.c ../c-pragma.c
  ../c-semantics.c ../c-lex.c ../c-dump.c ../c-pretty-print.c
  ../c-opts.c
  ${cxx_target_srcs}
  )

TARGET_LINK_LIBRARIES(cc1plus backend cpp iberty)
