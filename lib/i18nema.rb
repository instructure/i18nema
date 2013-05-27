require 'i18n'
require File.dirname(__FILE__) + '/i18nema/i18nema'

module I18nema
  class Backend
    include I18n::Backend::Base

    def store_translations(locale, data, options = {})
    end

    def available_locales
      init_translations unless initialized?
      I18nema.available_locales.map(&:to_sym)
    end

  protected
    def load_file(filename)
      type = File.extname(filename).tr('.', '').downcase
      raise I18n::UnknownFileType.new(type, filename) unless type == "yml"
      load_yml(filename)
    end

    def load_yml(filename)
      I18nema.load_yml_string File.read(filename)
    end

    def initialized?
      @initialized
    end

    def lookup(locale, key, scope = [], options = {})
      init_translations unless initialized?
      keys = I18n.normalize_keys(locale, key, scope, options[:separator])
      puts keys.inspect
      I18nema.direct_lookup(*keys)
    end

    def init_translations
      load_translations
      @initialized = true
    end
  end
end
