require 'syck'
require 'i18n'
require File.dirname(__FILE__) + '/i18nema/core_ext/hash'
require File.dirname(__FILE__) + '/i18nema/i18nema'

module I18nema
  module CoreMethods
    RESERVED_KEY_MAP = Hash[I18n::RESERVED_KEYS.map{|k|[k,true]}]

    def translate(locale, key, options = {})
      raise I18n::InvalidLocale.new(locale) unless locale
      entry = key && lookup(locale, key, options[:scope], options)

      if options.empty?
        entry = resolve(locale, key, entry, options)
      else
        count, default = options.values_at(:count, :default)
        # significant speedup over Hash#except
        values = options.reject { |key, value| RESERVED_KEY_MAP.key?(key) }
        entry = entry.nil? && default ?
          default(locale, key, default, options) : resolve(locale, key, entry, options)
      end

      throw(:exception, I18n::MissingTranslation.new(locale, key, options)) if entry.nil?
      # no need to dup, since I18nema gives us a new string

      entry = pluralize(locale, entry, count) if count
      entry = interpolate(locale, entry, values) if values
      entry
    end
  end

  class Backend
    include I18n::Backend::Base
    include CoreMethods # defined in a module so that other modules (e.g. I18n::Backend::Fallbacks) can override them

    def store_translations(locale, data, options = {})
      # TODO: make this moar awesome
      @initialized = true
      load_yml_string({locale => data}.deep_stringify_keys.to_yaml)
    end

    def init_translations
      load_translations
      @initialized = true
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
      keys = normalize_keys(locale, key, scope, options[:separator])
      direct_lookup(*keys)
    end

    def normalize_keys(locale, key, scope, separator = nil)
      separator ||= I18n.default_separator

      keys = [locale.to_s]
      keys.concat normalize_key(scope, separator) if scope && scope.size > 0
      keys.concat normalize_key(key, separator)
      keys
    end
  end
end
