EXTRA_DIST = ipush.spec rc.ipushd rc.ipushd.ini
install: install-ini

conftodir = "`echo $(sysconfdir)|sed -e 's|/usr/etc|/etc|'`"

initdir = "$(DESTDIR)$(conftodir)/rc.d/init.d"

sysconftodir = "$(DESTDIR)$(conftodir)"

install-ini:
	@echo install ipushd.ini ipushd to $(sysconftodir)
	@test -d $(sysconftodir)      || mkdir -p $(sysconftodir)
	@test -d $(initdir) || mkdir -p $(initdir)
	@test -f $(sysconftodir)/ipushd.ini \
		|| cp -p $(top_srcdir)/doc/rc.ipushd.ini $(sysconftodir)/ipushd.ini
	@test -f $(initdir)/ipushd \
		|| cp -p $(top_srcdir)/doc/rc.ipushd $(initdir)/ipushd
	@perl -i -p -e "s@/usr/local/sbin@$(prefix)/sbin@g" $(initdir)/ipushd
	@perl -i -p -e "s@/usr/local/etc@$(conftodir)@g" $(initdir)/ipushd
