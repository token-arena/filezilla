# Rules for the test code (use `make check` to execute)

# The S3 self-test compiles protocol sources that live outside this directory.
AUTOMAKE_OPTIONS = subdir-objects

if ENABLE_GUI
  MAYBE_GUI_TEST = gui_test
endif

if ENABLE_S3
  MAYBE_S3_SELFTEST = s3selftest
endif

if ENABLE_B2
  MAYBE_B2_SELFTEST = b2selftest
endif

if ENABLE_AZURE_BLOB
  MAYBE_AZURE_SELFTEST = azureselftest
endif

if ENABLE_GCS
  MAYBE_GCS_SELFTEST = gcsselftest
endif

if ENABLE_GDRIVE
  MAYBE_DRIVE_SELFTEST = driveselftest
endif

if ENABLE_ONEDRIVE
  MAYBE_GRAPH_SELFTEST = graphselftest
endif

if ENABLE_DROPBOX
  MAYBE_DROPBOX_SELFTEST = dropboxselftest
endif

if ENABLE_OSS
  MAYBE_OSS_SELFTEST = ossselftest
endif

if ENABLE_COS
  MAYBE_COS_SELFTEST = cosselftest
endif

if ENABLE_OBS
  MAYBE_OBS_SELFTEST = obsselftest
endif

TESTS = test $(MAYBE_GUI_TEST) $(MAYBE_S3_SELFTEST) $(MAYBE_B2_SELFTEST) $(MAYBE_AZURE_SELFTEST) $(MAYBE_GCS_SELFTEST) $(MAYBE_DRIVE_SELFTEST) $(MAYBE_GRAPH_SELFTEST) $(MAYBE_DROPBOX_SELFTEST) $(MAYBE_OSS_SELFTEST) $(MAYBE_COS_SELFTEST) $(MAYBE_OBS_SELFTEST)
check_PROGRAMS = $(TESTS)

# Standalone self-test for the S3 protocol logic (SigV4 signing, XML parsing).
# Links only against libfilezilla so it builds in seconds.
s3selftest_SOURCES = \
	s3selftest.cpp \
	../src/engine/s3/s3signer.cpp \
	../src/engine/s3/s3response.cpp \
	../src/pugixml/pugixml.cpp

s3selftest_CPPFLAGS = $(LIBFILEZILLA_CFLAGS) -DBUILDING_FILEZILLA
s3selftest_LDFLAGS = $(LIBFILEZILLA_LIBS)

# Standalone self-test for the Backblaze B2 protocol logic (JSON parsing).
b2selftest_SOURCES = \
	b2selftest.cpp \
	../src/engine/b2/b2json.cpp

b2selftest_CPPFLAGS = $(LIBFILEZILLA_CFLAGS) -DBUILDING_FILEZILLA
b2selftest_LDFLAGS = $(LIBFILEZILLA_LIBS)

# Standalone self-test for the Azure Blob logic layer (SharedKey signing).
azureselftest_SOURCES = \
	azureselftest.cpp \
	../src/engine/azure/azureblob.cpp \
	../src/pugixml/pugixml.cpp

azureselftest_CPPFLAGS = $(LIBFILEZILLA_CFLAGS) -DBUILDING_FILEZILLA
azureselftest_LDFLAGS = $(LIBFILEZILLA_LIBS)

# Standalone self-test for the Google Cloud Storage logic layer.
gcsselftest_SOURCES = \
	gcsselftest.cpp \
	../src/engine/gcs/gcsjson.cpp

gcsselftest_CPPFLAGS = $(LIBFILEZILLA_CFLAGS) -DBUILDING_FILEZILLA
gcsselftest_LDFLAGS = $(LIBFILEZILLA_LIBS)

# Standalone self-test for the Google Drive logic layer.
driveselftest_SOURCES = \
	driveselftest.cpp \
	../src/engine/gdrive/drivejson.cpp

driveselftest_CPPFLAGS = $(LIBFILEZILLA_CFLAGS) -DBUILDING_FILEZILLA
driveselftest_LDFLAGS = $(LIBFILEZILLA_LIBS)

# Standalone self-test for the Microsoft Graph (OneDrive) logic layer.
graphselftest_SOURCES = \
	graphselftest.cpp \
	../src/engine/onedrive/graphjson.cpp

graphselftest_CPPFLAGS = $(LIBFILEZILLA_CFLAGS) -DBUILDING_FILEZILLA
graphselftest_LDFLAGS = $(LIBFILEZILLA_LIBS)

# Standalone self-test for the Dropbox logic layer.
dropboxselftest_SOURCES = \
	dropboxselftest.cpp \
	../src/engine/dropbox/dropboxjson.cpp

dropboxselftest_CPPFLAGS = $(LIBFILEZILLA_CFLAGS) -DBUILDING_FILEZILLA
dropboxselftest_LDFLAGS = $(LIBFILEZILLA_LIBS)

# Standalone self-test for the Alibaba Cloud OSS logic layer.
ossselftest_SOURCES = \
	ossselftest.cpp \
	../src/engine/oss/ossrequest.cpp \
	../src/pugixml/pugixml.cpp

ossselftest_CPPFLAGS = $(LIBFILEZILLA_CFLAGS) -DBUILDING_FILEZILLA
ossselftest_LDFLAGS = $(LIBFILEZILLA_LIBS)

# Standalone self-test for the Tencent Cloud COS logic layer.
cosselftest_SOURCES = \
	cosselftest.cpp \
	../src/engine/cos/cossign.cpp \
	../src/pugixml/pugixml.cpp

cosselftest_CPPFLAGS = $(LIBFILEZILLA_CFLAGS) -DBUILDING_FILEZILLA
cosselftest_LDFLAGS = $(LIBFILEZILLA_LIBS)

# Standalone self-test for the Huawei Cloud OBS logic layer.
obsselftest_SOURCES = \
	obsselftest.cpp \
	../src/engine/obs/obsrequest.cpp \
	../src/pugixml/pugixml.cpp

obsselftest_CPPFLAGS = $(LIBFILEZILLA_CFLAGS) -DBUILDING_FILEZILLA
obsselftest_LDFLAGS = $(LIBFILEZILLA_LIBS)

test_SOURCES = \
	test.cpp \
	dirparsertest.cpp \
	localpathtest.cpp \
	serverpathtest.cpp

test_CPPFLAGS = -I$(top_builddir)/config
test_CPPFLAGS += $(LIBFILEZILLA_CFLAGS)
test_CPPFLAGS += $(LIBFZSSH_CFLAGS)
test_CXXFLAGS = $(CPPUNIT_CFLAGS)

test_LDFLAGS = ../src/engine/libfzclient-private.la
test_LDFLAGS += $(LIBFILEZILLA_LIBS)
test_LDFLAGS += $(LIBFZSSH_LIBS)
test_LDFLAGS += $(PUGIXML_LIBS)
test_LDFLAGS += $(LIBGNUTLS_LIBS)
test_LDFLAGS += $(IDN_LIB)
test_LDFLAGS += $(LIBSQLITE3_LIBS)
test_LDFLAGS += $(CPPUNIT_LIBS)

test_DEPENDENCIES = ../src/engine/libfzclient-private.la

if ENABLE_GUI

gui_test_SOURCES = \
	cmpnatural.cpp \
	gui_test.cpp

gui_test_CPPFLAGS = -I$(top_builddir)/config
gui_test_CPPFLAGS += $(LIBFILEZILLA_CFLAGS)
gui_test_CPPFLAGS += $(LIBFZSSH_CFLAGS)
gui_test_CPPFLAGS += $(WX_CPPFLAGS)
gui_test_CXXFLAGS = $(WX_CXXFLAGS_ONLY) $(CPPUNIT_CFLAGS)

gui_test_LDFLAGS = ../src/engine/libfzclient-private.la
gui_test_LDFLAGS += $(LIBFILEZILLA_LIBS)
gui_test_LDFLAGS += $(LIBFZSSH_LIBS)
gui_test_LDFLAGS += $(LIBGNUTLS_LIBS)
gui_test_LDFLAGS += $(WX_LIBS)
gui_test_LDFLAGS += $(IDN_LIB)
gui_test_LDFLAGS += $(LIBSQLITE3_LIBS)
gui_test_LDFLAGS += $(CPPUNIT_LIBS)
gui_test_LDFLAGS += $(PUGIXML_LIBS)
gui_test_DEPENDENCIES = ../src/engine/libfzclient-private.la

endif

