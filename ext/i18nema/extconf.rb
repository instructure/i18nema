require 'mkmf'
dir_config "i18nema/i18nema"
have_header "st.h"
$CFLAGS << " -std=c99"
create_makefile 'i18nema/i18nema'
