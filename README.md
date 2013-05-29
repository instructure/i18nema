# I18nema

Fast I18n backend to keep things running smoothly.

I18nema is a drop-in replacement for I18n::Backend::Simple, for faster
lookups and quicker gc runs. Translations are stored outside of the ruby
heap, and lookups happen in C (rather than the usual inject on nested
ruby hashes).

## How do I use it?

    gem 'i18nema'

and then do something like this in an initializer:

    I18n.backend = I18nema::Backend.new

As with I18n::Backend::Simple, you can pull in additional features, e.g.

    I18nema::Backend.send(:include, I18n::Backend::Fallbacks)

## Notes

You should probably make sure translations are loaded before you fork. In
an initializer, just do `I18n.backend.init_translations`
