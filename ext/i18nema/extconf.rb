require 'mkmf'
have_header("st.h")
#have_library("libsyck-dev")
dir_config('i18nema/i18nema')
$CFLAGS << " -g"
create_makefile('i18nema/i18nema')
