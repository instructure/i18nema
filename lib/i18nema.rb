require 'i18n'
require File.dirname(__FILE__) + '/i18nema/core_ext/hash'
require File.dirname(__FILE__) + '/i18nema/i18nema'

module I18nema
  class Backend
    include I18n::Backend::Base

    def store_translations(locale, data, options = {})
      # TODO: make this moar awesome
      @initialized = true
      load_yml_string({locale => data}.deep_stringify_keys.to_yaml)
    end

  protected
    def load_file(filename)
      type = File.extname(filename).tr('.', '').downcase
      raise I18n::UnknownFileType.new(type, filename) unless type == "yml"
      load_yml(filename)
    end

    def load_yml(filename)
      load_yml_string File.read(filename)
    end

    def initialized?
      @initialized
    end

    def lookup(locale, key, scope = [], options = {})
      init_translations unless initialized?
      keys = I18n.normalize_keys(locale, key, scope, options[:separator])
      direct_lookup(*keys)
    end

    def init_translations
      load_translations
      @initialized = true
    end
  end
end
