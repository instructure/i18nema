class Hash
  def deep_stringify_keys
    Hash[map{ |key, value|
      value = value.deep_stringify_keys if value.is_a?(Hash)
      [key.to_s, value]
    }]
  end unless Hash.method_defined?(:deep_stringify_keys)
end
