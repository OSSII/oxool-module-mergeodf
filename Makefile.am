MODULE_NAME = MergeODF
MODULE_DIR = @LOOLWSD_MODULE_DATA_DIR@/${MODULE_NAME}

include_paths = -I$(abs_top_builddir)/src/include \
		-I$(abs_top_builddir)/common \
		-I$(abs_top_builddir)/net \
		-I$(abs_top_builddir)/wsd

EXTRA_DIST = admin/*

install-data-hook:
	mkdir -p $(DESTDIR)$(MODULE_DIR)
	cp -aR admin $(DESTDIR)$(MODULE_DIR)/

uninstall-hook:
	rm -fr $(DESTDIR)$(MODULE_DIR)

configdir = @LOOLWSD_MODULE_CONFIG_DIR@
dist_config_DATA = ${MODULE_NAME}.xml

# Module install path
moduledir = @LOOLWSD_MODULE_DIR@
module_LTLIBRARIES = MergeODF.la

MergeODF_la_CPPFLAGS = -pthread ${include_paths}

MergeODF_la_LDFLAGS = -avoid-version -module -lPocoDataSQLite

MergeODF_la_SOURCES = src/MergeODF.cpp \
		      src/MergeODFParser.cpp

noinst_HEADERS = src/MergeODF.h \
                 src/MergeODFParser.h
