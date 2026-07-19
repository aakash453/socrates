Pod::Spec.new do |s|
  s.name         = "SocratesEngine"
  s.version      = "0.1.0"
  s.summary      = "Edge AI Runtime Engine"
  s.homepage     = "https://github.com/multiverse/socrates"
  s.license      = { :type => "MIT" }
  s.authors      = "Multiverse"
  s.source       = { :git => "https://github.com/multiverse/socrates.git", :tag => s.version }
  s.vendored_frameworks = "build/package/SocratesEngine.xcframework"
  s.platform = :ios, '16.0'
end
