require 'rake'
require 'rake/extensiontask'

desc 'Default: run unit tests.'
task :default => :test

require 'rake/testtask'
desc 'Test the immigrant plugin.'
Rake::TestTask.new(:test) do |t|
  t.libs << 'lib'
  t.libs << 'test'
  t.pattern = 'test/**/*_test.rb'
  t.verbose = true
end
Rake::Task[:test].prerequisites << :compile

Rake::ExtensionTask.new('i18nema') do |ext|
  ext.lib_dir = File.join('lib', 'i18nema')
end
