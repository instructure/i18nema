RUBYONEX = RUBY_VERSION < "2.0"

Gem::Specification.new do |s|
  s.name = RUBYONEX ? "i18nema19" : "i18nema"
  s.version = '0.0.8'
  s.summary = "fast i18n backend that doesn't stop up the garbage collector"
  s.description = "drop-in replacement for I18n::Backend::Simple for faster lookups and quicker gc runs. translations are stored outside of the ruby heap"

  s.required_ruby_version     = RUBYONEX ? '>= 1.9.3' : '>= 2.0.0'
  s.required_rubygems_version = '>= 1.3.5'

  s.author            = 'Jon Jensen'
  s.email             = 'jon@instructure.com'
  s.homepage          = 'http://github.com/instructure/i18nema'

  s.extensions = ['ext/i18nema/extconf.rb']
  s.files = %w(Rakefile README.md) + Dir['ext/**/*.{c,h,rb}'] + Dir['lib/**/*.rb'] + Dir['test/**/*.rb']
  s.add_dependency('syck', '~> 1.0') unless RUBYONEX
  s.add_dependency('i18n', '>= 0.5')
  s.add_development_dependency('rake-compiler', '~> 0.8.0')
end
