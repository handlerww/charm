-include ../../../../../common.mk
-include ../../../../../../include/conv-mach-opt.mak
CHARMC=../../../../../../bin/charmc $(OPTS)

all: simple_put

OBJS = simple_put.o

simple_put: $(OBJS)
	$(CHARMC) -language charm++ -o simple_put $(OBJS)

cifiles: simple_put.ci
	$(CHARMC)  simple_put.ci
	touch cifiles

simple_put.o: simple_put.C cifiles
	$(CHARMC) -c simple_put.C

test: all
	$(call run, +p1 ./simple_put 20 )
	$(call run, +p2 ./simple_put 20 )
ifeq ($(CMK_USE_CMA),1)
	$(call run, +p2 ./simple_put 20 +noCMAForZC)
endif

clean:
	rm -f *.decl.h *.def.h conv-host *.o simple_put charmrun cifiles
