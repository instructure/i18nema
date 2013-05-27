require 'helper'
require 'yaml'

class I18nemaTest < Test::Unit::TestCase
  def setup
    @data = {
      "en" => {
        "foo" => {
          "bar" => "lol"
        },
        "baz" => %w{
          asdf
          qwerty
        }
      }
    }
    I18nema.load_yml_string @data.to_yaml
  end

  def teardown
    I18nema.reset!
  end

  def test_yaml_parity
    assert_equal @data, I18nema.direct_lookup
  end

  def test_scoping
    assert_equal({"bar" => "lol"},
                 I18nema.direct_lookup("en", "foo"))
    assert_equal "lol",
                 I18nema.direct_lookup("en", "foo", "bar")
    assert_equal nil,
                 I18nema.direct_lookup("poo")
  end

  def test_merging
    I18nema.load_yml_string({
      "en" => {
        "foo" => "replaced!",
        "wat" => "added!"
      }
    }.to_yaml)
    assert_equal "replaced!",
                 I18nema.direct_lookup("en", "foo")
    assert_equal ["asdf", "qwerty"],
                 I18nema.direct_lookup("en", "baz")
    assert_equal "added!",
                 I18nema.direct_lookup("en", "wat")
  end

  def test_reset
    I18nema.reset!
    assert_equal I18nema.direct_lookup, nil
  end

  def test_available_locales
    I18nema.load_yml_string("es:\n  foo: hola")
    assert_equal ['en', 'es'],
                 I18nema.available_locales.sort
  end
end
