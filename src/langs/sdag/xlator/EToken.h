/*****************************************************************************
 * $Source$
 * $Author$
 * $Date$
 * $Revision$
 *****************************************************************************/

#ifndef _EToken_H_
#define _EToken_H_

enum EToken {
   DEFAULT=1

  ,IDENT=2
  ,LBRACE=3
  ,RBRACE=4
  ,LB=5
  ,RB=6
  ,LP=7
  ,RP=8
  ,COLON=9
  ,STAR=10
  ,CHAR=11
  ,STRING=12
  ,NEW_LINE=13
  ,CLASS=14
  ,ENTRY=15
  ,SDAGENTRY=16
  ,OVERLAP=17
  ,WHEN=18
  ,IF=19
  ,WHILE=20
  ,FOR=21
  ,FORALL=22
  ,ATOMIC=23
  ,COMMA=24
  ,ELSE=25
  ,SEMICOLON=26

  ,BRACE_MATCHED_CPP_CODE=100
  ,PAREN_MATCHED_CPP_CODE=101
  ,INT_EXPR=102
  ,WSPACE=103
  ,SLIST=104
  ,ELIST=105
  ,OLIST=106
};

#endif /* _EToken_H_ */
